# STATUS — 2026-05-26 セッションメモ

このセッション (2026-05-26) で btstack/ 周りに入った変更と、Phase A 着手前に
踏まえておきたい既存ファイルへの索引。次に作業するときの起点として読む。

## このセッションで増えたファイル

| ファイル | 概要 |
|---|---|
| [`03-dm250-bt-pinmap.md`](03-dm250-bt-pinmap.md) | DM250 AP6212A の UART / GPIO 配線。jcs/linux-dm250 dts と jcs/openbsd-src rk3128 ブランチから抽出 |
| [`04-outbound-intercept.md`](04-outbound-intercept.md) | pomera 側 outbound intercept (pf divert-to vs tun(4)) の検証設計。Phase A Step 6 の実施手順 |
| [`05-pairing-runbook.md`](05-pairing-runbook.md) | BT 再ペアリング手順 (2026-05-31)。 advertise on + 人手 confirm のセキュア設計を運用するための手順書 |
| [`etc/firmware.README.md`](etc/firmware.README.md) | BT patchram (配布元 `BCM43430A1.hcd`、 kernel 期待 `BCM4343A1.hcd`) の入手元と SHA256 確定 |
| [`etc/unbound.conf.sample`](etc/unbound.conf.sample) | DoT で Tailscale bootstrap を通す unbound 設定雛形 |
| [`PROTOCOL.md`](PROTOCOL.md) | mux protocol v0 (draft)。panctl と android_app で共有 |

[DESIGN.md](DESIGN.md) と [01-hci-bringup.md](01-hci-bringup.md) / [02-btstack-port-notes.md](02-btstack-port-notes.md) は既存のまま、本セッションでは触っていない。

## 重要発見 (DESIGN.md の前提を一部覆す)

### bring-up 分岐確定 (2026-05-26 ソース読解)

- 当初の前提: 「BT bring-up (GPIO 制御 + patchram 投入 + baud 切替) は全部 panctl 側 userland で書く」
- 判明した事実: jcs (joshua stein) が **OpenBSD カーネル側に既に `bcmbt_fdt.c` という BT bring-up ドライバを書いている**。 `OF_is_compatible(..., "brcm,bcm43438-bt")` で Linux dts のバインディングを直接 consume する。
- **ソースを最後まで読んだ結果 ([`bcmbt_fdt.c`](https://github.com/jcs/openbsd-src/blob/rk3128/sys/dev/fdt/bcmbt_fdt.c))、 kernel の挙動が完全に確定**:
  1. attach 時 (mountroot 直後) に **一発処理**で以下を済ませる
     - `shutdown-gpios` を OFF→200ms→ON（チップを power-cycle）
     - `uart-rts-gpios` を GPIO mode に切り替えてパルス → UART mode に戻す（RTS pulse でチップを wake）
     - `device-wakeup-gpios` を ON
     - UART を 115200 8N1 で初期化
     - `HCI Reset` (0x03 0x0c)
     - `HCI Vendor Download Minidriver` (0x2e 0xfc)
     - `loadfirmware(9)` で **`/etc/firmware/BCM4343A1.hcd`** を読み込み（`BCM4343A1` であって `BCM43430A1` ではない、 hardcoded、 `/* TODO: per-chipset firmware files */`）、 各エントリを HCI コマンドとして UART に流す
     - post-firmware reset を 250ms 待つ
     - UART を再初期化（115200 のまま、 baud 切替なし）
     - `HCI Reset` 再投入
     - `HCI Read BD_ADDR` (0x09 0x10) で MAC アドレスを取得して `bcmbt0: address ...` で printf
  2. **以後何もしない**。 `struct cfdriver bcmbt_cd = { …, "bcmbt", DV_DULL };` で **cdev を作らない**
  3. parent は **`com(4)` (uart0)** であり、 attach 後も userland は **標準の `/dev/cua00`** で UART にアクセスできる（115200 で BT chip が HCI ready 状態のまま座っている）
- **結果**: 3 択 (A)/(B)/(C) のうち厳密にはどれにも当てはまらない、 **「(A) を patchram まで拡張、 ただし transport は元から (A) と同じ標準 com(4) `/dev/cua00`」** という分岐。 userland への含意は (C) に近い:

| Phase B サブステップ | 実施場所 |
|---|---|
| GPIO bring-up | **kernel** (`bcmbt_fdt.c`) |
| UART 初期化 | **kernel** (115200 8N1 で固定) |
| HCI Reset / Download Minidriver | **kernel** |
| patchram (`BCM4343A1.hcd`) | **kernel** (`/etc/firmware/` に置いてあるものを `loadfirmware(9)`) |
| baud 切替 | **不要** (115200 のまま運用) |
| BD_ADDR 読み出し | **kernel** (起動メッセージに出る) |
| HCI transport | **userland** (BTstack `port/posix-h4` を `/dev/cua00` で開く) |
| L2CAP / SDP / RFCOMM | **userland** (BTstack 通常通り) |
| BCM chipset init script (`btstack_chipset_bcm_*`) | **不要** (`chipset/none` で OK) |

- **firmware ファイル名の罠**: 配布元 RPi では `BCM43430A1.hcd` (チップ型番)、 kernel は `BCM4343A1.hcd` (patchram 系列名) を `loadfirmware()` に渡す。 取得後 **リネーム必須**。 詳細 [`etc/firmware.README.md`](etc/firmware.README.md) と openBSD 側 `install.md §6.2`
- 反映済み: [`DESIGN.md`](DESIGN.md) §1.2 §3.B、 [`etc/firmware.README.md`](etc/firmware.README.md)、 [`02-btstack-port-notes.md`](02-btstack-port-notes.md) §2、 [`03-dm250-bt-pinmap.md`](03-dm250-bt-pinmap.md) §5

### 実機 (<pomera-host>) で bring-up が止まる: post-firmware HCI Reset failed (2026-05-29)

`/etc/firmware/BCM4343A1.hcd` (SHA `c096ad4a...`、 30049 bytes、 RPi-Distro/bluez-firmware master) を配置して boot した結果、 `bcmbt_fdt.c` の挙動が以下で停止:

```
bcmbt0 at com0
bcmbt0: post-firmware HCI reset failed
```

ソースで言うと:

```c
/* wait for chip reset after firmware download */
delay(250000);                     // ← 250ms 待つ
/* re-initialize UART after chip reset */
bcmbt_uart_init(sc, 115200);
/* drain any boot diagnostic data */
while (bcmbt_uart_getc(sc, &junk, 10000) == 0) continue;
/* send post-firmware HCI Reset */
if (bcmbt_send_cmd(sc, hci_reset, sizeof(hci_reset)) != 0 ||
    bcmbt_recv_evt(sc, resp, sizeof(resp), &rlen) != 0) {  // ← 1 秒以内に応答が来ず失敗
    printf("%s: post-firmware HCI reset failed\n", ...);
    return;
}
```

#### 切り分け (userland 検証 2026-05-29)

ssh で <pomera-host> に入って `/dev/cua00` 経由で chip と話す試験を実施:

1. **kernel が bring-up 失敗した後の chip 状態**: 115200 で HCI Reset を投げても応答ゼロ、 listen して受動的に待っても 0 bytes/3s
2. **modem control bits**: DTR=1 RTS=1 CTS=1（電気は健全、 chip 側も送信許可状態）
3. **baud rate 切替仮説**: patchram (.hcd) を `od` で精査、 `18 fc 06` (Update Baud Rate, opcode 0xfc18) コマンドは含まれず、 末尾は `4e fc 04 ff ff ff ff` (Launch RAM) のみ。 つまり baud 変更は起きていないはず
4. **firmware を一旦避けて再起動 → kernel は firmware load fail で抜ける → chip は Download Minidriver まで進んで patchram 待機モード → userland から patchram 流し込み試行**: 1 個目の Vendor Write RAM (0xfc4c) に対しても応答無し
5. **userland から GPIO で BT_REG_ON を toggle して power-cycle 試行**: kernel が GPIO を握ったままで `/dev/gpio3` の pin 21 を `gpioctl` で操作できず (`Operation not permitted`)、 power-cycle 不可

#### 結論 (現時点の推測)

最有力: **`delay(250000)` の 250ms post-firmware 遅延が DM250 個体の chip には短すぎる**。 BCM43430A1 系の chip は patchram 投入後の internal reset に最大 1 秒以上要する個体報告あり。 kernel が drain + post-fw HCI Reset を投げるタイミングで chip がまだ完全に上がっておらず、 そこで応答が落ちると以降 stuck になる。

次善: silicon revision が patchram と微妙にずれている可能性。 RPi 配布の `BCM43430A1.hcd` は AP6212A の "A1" リビジョンを想定しているが、 DM250 の個体が異なる sub-revision の可能性。

#### 次にやること (別セッション、 panctl 着手前か並行で)

| 優先度 | 対処 | 概要 |
|---|---|---|
| 高 | `bcmbt_fdt.c` patch | `delay(250000)` → `delay(2000000)` (2s) に拡張、 もしくは post-fw HCI Reset を 5 回 retry にする。 jcs の rk3128 ブランチに対する PR ネタ |
| 中 | 別の firmware を試す | armbian-firmware / cypress 系の `BCM4343A1.hcd` / `BCM43430A1.hcd` で SHA 違いのバージョンを順に試す |
| 低 | userland から chip 強制リセット可能化 | DM250 用 gpio(4) 公開 (現状 0 pins 全部 kernel 占有)、 もしくは `device-wakeup-gpios` を userland から触る ioctl |

panctl 側は **chip が HCI ready 状態に到達しないと一切動かせない**（`/dev/cua00` 経由で HCI コマンドが返ってこないので）。 まずこの kernel patch を入れて bring-up を完走させるのが先決。

#### 続報: BT firmware 配置で Wi-Fi が落ちる (2026-05-29 後段、 実機追試)

firmware を本来位置に戻して再起動した結果、 **Wi-Fi (bwfm0) が
association 不能** になることを確認:

| firmware 状態 | bcmbt 挙動 | bwfm 信号 | bwfm status |
|---|---|---|---|
| `/etc/firmware/BCM4343A1.hcd` 配置 | post-fw HCI reset 失敗、 chip stuck | -73dBm | `no network` (auth/association 失敗) |
| firmware 避け (`.bak`) | firmware load fail で early return、 chip は Download Minidriver 直前 | -65dBm | `active`、 <dm250-lan-ip> 取得 |

原因仮説: AP6212A は **Wi-Fi (BCM43430) と BT (BCM43438) が同じシリコンダイ**で
共有 RF front-end と coexistence pins (BT_ACTIVE/WL_ACTIVE) で TDM。
post-fw HCI Reset 失敗後の BT chip stuck 状態で coex pin が「BT TX 中」
相当を assert し続けると、 Wi-Fi 側が RF 時間を取れず association できない。
さらに -73dBm への信号低下も、 stuck 状態の BT 側内部の発振が共有 PLL を
乱している可能性。

**この発見が panctl 設計に効くポイント**:

1. **BT 単独で「壊れた」状態は Wi-Fi にも飛び火する**。 panctl 着手前に
   bring-up を確実に成功させないと、 開発機自体が Wi-Fi (= 開発 ssh 経路)
   を失う。 → kernel patch (`delay` 拡張) を先に当てる
2. **Wi-Fi 検証期間中は BT を完全に attach させない方が安全**。 firmware 不
   配置にしておけば `bcmbt0: failed to load firmware` 1 行で済み、 chip は
   触られず Wi-Fi 健全
3. **将来 panctl をテストするフェーズ**でも、 BT bring-up が失敗するパスでは
   Wi-Fi 経路で母艦ログを送れない前提で組む（local log にだけ書く、
   など）。 Wi-Fi 越しに realtime debug したいなら kernel patch 必須

#### 実機状態保全

最終的に `/etc/firmware/BCM4343A1.hcd` は **`.bak` のまま** 残置
(2026-05-29 12:29 JST)。 `bcmbt0: failed to load firmware BCM4343A1.hcd (error 2)`
が dmesg に出続けるが、 Wi-Fi / SSH / userland 全般は安定。
ssh <your-pomera-user>@<dm250-lan-ip> で母艦 Mac から鍵認証 + doas nopass で操作可能。

#### 採用運用方針 (2026-05-29 当初想定 → 同日 Plan 2 実機検証で再検討中)

検討した選択肢:

| Plan | 内容 | 採用判断 |
|---|---|---|
| 1 | `.bak` 固定。 kernel は bcmbt を attach + chip を power-cycle するが patchram を投入しない → chip は Download Minidriver 待機の低電力状態 → Wi-Fi 干渉なし、 dmesg に `bcmbt0 at com0` は出る | **当面の標準** |
| 2 | `bcmbt_fdt.c` の `delay(250000)` を 2s 程度に拡張する kernel patch → patchram 完走 → BT 機能フル使用可、 chip 内部 hardware coex で Wi-Fi/BT TDM 自動制御 | **同日実機適用済 → 2s では bring-up 完走せず**。 別 delay 値 / retry / 代替 firmware を検証する Plan 2.x が必要 |
| 3 | userland HCI Sleep daemon | Plan 2 で chip 内 coex が機能すれば不要、 保留 |
| (検討中断) | `btmode` / `btauto` 等の userland 切替ツール | 採用せず。 reboot 必須の根本制約を変えられず、 道具を増やすだけになる |

理由: 「ドライバ load + 電力制限」という user 要件は **Plan 1 そのもの**である。 bcmbt
attach は無条件で起き、 patchram を投入しなければ chip は active モードに遷移
しない。 すなわち現状で既に「driver loaded, low power」を満たしている。 BT
機能が実際に必要になるタイミング (panctl 開発開始) で Plan 2 に移行する。

#### Plan 2 実機適用結果 (2026-05-29、 二段階)

##### 第 1 段階 (14:25 JST): kernel patch のみで RPi `BCM43430A1.hcd` を試行

| 項目 | 結果 |
|---|---|
| ビルド | ✓ pomera 上 native build 73 分で完走 (link 直前に `/usr` 98% で焦った、 `.git` 274MB 削って凌いだ) |
| 新 kernel boot | ✓ `OpenBSD <pomera-host>.my.domain 7.9 GENERIC#0 armv7` |
| bcmbt bring-up | ✗ **依然 `post-firmware HCI reset failed`** → 2s 延長だけでは不十分 |
| Wi-Fi 共存 | ✓ `bwfm0` 接続 active、 -57dBm (BT 失敗状態でも巻き添えは出なかった) |

→ kernel patch だけでは bring-up 不完走。 同時に firmware バージョンの違いも疑う。

##### 第 2 段階 (14:36 JST): kernel patch ＋ armbian の AP6212A 専用 firmware に差し替え

| 項目 | 結果 |
|---|---|
| firmware 差し替え | RPi `BCM43430A1.hcd` (30049 bytes、 SHA `c096ad4a…`) → armbian `ap6212/bcm43438a1.hcd` (33376 bytes、 SHA `d396912a…`) |
| reboot 後 dmesg | ✓ **`bcmbt0: address XX:XX:XX:XX:XX:XX`** ← BD_ADDR まで読み出し成功＝ kernel 側 bring-up **完走** |
| Wi-Fi 共存 | ✓ `bwfm0` active、 -57dBm、 <dm250-lan-ip> (BT 完走と並行で全く影響なし) |
| userland `/dev/cua00` 経由 HCI Reset | ✗ 無応答 (chip が auto-sleep に入った推定、 BTstack の `chipset_bcm` 統合段階で wake シーケンスは解決される予定) |

##### つまり Plan 2 達成条件

**kernel patch (delay 2s)** + **armbian の `ap6212/bcm43438a1.hcd`** の組み
合わせで chip が HCI ready 状態に到達する。 RPi 配布版 (`BCM43430A1.hcd`、
SHA `c096ad4a…`) は DM250 個体の chip silicon revision とパッチが微妙に
合わない様子で、 patchram 投入後の reset 応答が消える。 armbian 版は
**AP6212A モジュールを使う Allwinner / Rockchip 系 SBC 向けに調整された
patchram** で、 DM250 (AMPAK AP6212A + RK3128) の chip に正しく適合。

成果物バックアップ:
- [`../kernel-patches/bcmbt-delay-2s.patch`](../kernel-patches/bcmbt-delay-2s.patch) — unified diff
- [`../kernel-patches/bcmbt_fdt.c.patched`](../kernel-patches/bcmbt_fdt.c.patched) — 適用後ソース全文
- ビルド済 kernel (`bsd.armv7.delay-2s`, SHA `1aa585ec…`) — GitHub Release で配布
- 入れ替え前の jcs オリジナル (`bsd.armv7.jcs-original`, SHA `dd5c9394…`) — 同 release アセット
- **動作確認済 BT firmware** `BCM4343A1.hcd.armbian-ap6212` (SHA `d396912a…`、 armbian の AP6212A 専用版) — 同 release アセット

別の DM250 に同じことをしたい時は
[`../kernel-patches/README.md`](../kernel-patches/README.md) のルート
A (prebuilt 投入、 5 分) かルート B (再 build、 1 時間) に従う。 firmware は
本ファイル `BCM4343A1.hcd.armbian-ap6212` をそのまま `/etc/firmware/BCM4343A1.hcd`
に配置する (リネーム不要、 中身がもう正しい patchram)。

#### 次の課題 (Plan 2 達成後の残タスク)

| # | やること | 着手フェーズ |
|---|---|---|
| 1 | ~~userland から chip を wake → HCI コマンド送出~~ ← **2026-05-29 15:00 解決済**: RTS pulse (off 50ms → on) で chip wake → HCI Reset 応答 OK。 詳細は本ファイル §「userland HCI 通信成立」 |
| 2 | RTS pulse 後の BD_ADDR が `aa:aa:aa:aa:aa:aa` (factory default) に戻る件 — soft-reset 副作用？ BTstack の chipset_bcm 統合段階で OTP 再読込シーケンスが入る想定 |
| 3 | jcs の rk3128 ブランチに upstream PR (`delay` 値拡張＋ firmware filename TODO 解決) | 将来 |
| 4 | armbian の `bcm43438a1.hcd` のライセンス・配布条件確認 (repo に置いていいか) | コミット前 |
| 5 | kernel patch が本当に必要だったか検証 (firmware 差し替えだけで動くなら patch 不要、 jcs オリジナル kernel + armbian firmware で動作確認) | 余裕がある時 |
| 6 | クロスビルド成果物 (panctl/build-armv7/test_frame_armv7) が segfault するのを修復 | panctl 本実装入る前 |

#### userland HCI 通信成立 (2026-05-29 15:00 JST)

Plan 2 で kernel bring-up は完走したが、 直後の `/dev/cua00` 経由 HCI Reset
は無応答だった件の続報。 `tools/bcm_uart_probe.c`
(初版、 wake bytes のみ) と `tools/bcm_uart_probe2.c`
(改良版、 modem control 操作) ※ 本リポ外の bring-up scratch、 で実機検証した結果:

| 戦略 | 結果 |
|---|---|
| s1: wake bytes (0xff × 10) + HCI Reset | ✗ 無応答 |
| s2: **RTS deassert 50ms → reassert → HCI Reset** | ★ **応答 OK**：`04 0e 04 01 03 0c 00` (HCI Command Complete) |
| s3: DTR pulse | s2 と同等 |

kernel が bring-up 終わった後、 chip は ULP sleep に入っており、 **host 側
RTS pulse が wake トリガ**になっている。 BCM 系の慣習的な wake プロトコル。
BTstack の `chipset_bcm` が同等のシーケンスを送るので panctl 統合時には透過。

probe2 で続けて `HCI Read BD_ADDR` を送ると `aa:aa:aa:aa:aa:aa` (BCM 系の
factory default) が返ってきた。 kernel 起動直後の `bcmbt0: address
XX:XX:XX:XX:XX:XX` とは違う値 — RTS pulse + HCI Reset で chip が soft-reset
され OTP からの BD_ADDR ロードがクリアされたと推定。 panctl 側で
`btstack_chipset_bcm` の init script (BD_ADDR set 含む) を流せば解消する想定。

成果物:
- `tools/bcm_uart_probe.c` — シンプル版 (wake bytes、 HCI Reset、 Read BD_ADDR、 Read Local Version Info)
- `tools/bcm_uart_probe2.c` — 改良版 (4 戦略: as-is / RTS pulse / DTR pulse / wake bytes + retry x5)

両方 pomera 上で `cc -O -Wall -o bcm_uart_probe bcm_uart_probe.c` で native
build、 `doas` 起動。 panctl の H4 transport 実装の参考にできる。

#### BTstack fetch 完了 (2026-05-29 15:00)

`tools/fetch-btstack.sh` で BTstack v1.6.2 を `third_party/btstack/` に
clone 済 (.git 込みで 242MB、 ignored 対象に追加予定)。 これで posix-h4
port のソースが揃ったので、 panctl/Makefile に panctl-h4 ターゲットを足す
段が次のセッションのスコープ。 BTstack 本体の build は OpenBSD で実機 build
する想定 (cross-build は test_frame_armv7 の segfault 解決が前提)。

#### ★ BTstack smoke test 完走 = panctl 前提全証明 (2026-05-29 16:02 JST)

DM250 で BTstack v1.6.2 を build → 実行 → BT chip と HCI 通信成立、
`HCI_STATE_WORKING` まで到達。 詳細・成果物は
[`panctl-h4-bringup/README.md`](panctl-h4-bringup/README.md):

| 観察 | 値 |
|---|---|
| build 時間 (pomera native, 66 sources) | 数分 |
| binary size | 553 KB |
| HCI init コマンド数 (cmd-complete) | 30+ (Reset, Version, BD_ADDR, Buffer Size, Local Features, Set Event Mask, etc.) |
| 到達 state | `HCI_STATE_WORKING` (2) |
| reported BD_ADDR | `aa:aa:aa:aa:aa:aa` (BCM factory default、 OTP 復元は panctl で別途) |
| chip wake 制御 | BTstack の `chipset_bcm` が自動でやる (我々が `bcm_uart_probe2.c` で発見した RTS pulse 相当のシーケンスは内蔵) |

これで以下が**全部証明済み**:

1. BTstack core が OpenBSD/armv7 で素直に build 通る (errno.h + strings.h の include 追加 2 件のみで OK)
2. H4 transport (`btstack_uart_posix.c`) が `/dev/cua00` 経由で BT chip と話せる
3. `chipset_bcm` の BCM 系初期化が DM250 個体に対しても通る
4. 永続化 TLV / RFCOMM / SDP の前提となる HCI run loop が回る

成果物:
- `panctl-h4-bringup/btstack_h4_smoke.c` — minimal main (HCI 起動 → state=WORKING で BD_ADDR 表示)
- `panctl-h4-bringup/build_smoke.sh` — pomera 上で 1 cc で全部 link する build スクリプト (audio/mesh/不要 profile 除外)
- `panctl-h4-bringup/README.md` — 再現手順 + 残課題
- [`../kernel-patches/btstack-v1.6.2-openbsd-compat.patch`](../kernel-patches/btstack-v1.6.2-openbsd-compat.patch) — BTstack 側の OpenBSD 互換性 patch (errno.h + strings.h の include 2 件)

#### 次フェーズ (panctl 本実装)

| # | やること |
|---|---|
| 1 | smoke の枠組みを `panctl/` 配下の main.c に転写、 mux.c/frame.c/ipv4.c と link |
| 2 | RFCOMM client を spp_streamer_client ベースで実装 (Android アプリ側と SDP browse → channel discover → 接続) |
| 3 | mux protocol v0 の encoder/decoder を panctl 側 mux.c に実装 (PROTOCOL.md 参照) |
| 4 | RFCOMM ↔ mux の橋渡し |
| 5 | pf divert socket で out-bound TCP/UDP を横取り → mux に乗せる |
| 6 | Android 側 mux server と E2E echo |
| 7 | rc.d 化 + 永続化 (BD_ADDR / link key / TLV) |
| 8 | (任意) BTstack cross-compile 環境整備 (sysroot 経由) — 反復開発を早くする |
| 9 | Tailscale を pomera に入れて mux 経由で母艦 tmux に attach できる状態へ |

#### 実機状態 (2026-05-29 14:39 JST、 Plan 2 達成)

- `/bsd` = **Plan 2 patch 済 (`bsd.armv7.delay-2s` SHA `1aa585ec…`)**
- `/bsd.jcs-original` = 旧版バックアップ (rollback 用、 SHA `dd5c9394…`)
- `/bsd.booted` = 起動時に load した kernel (Plan 2 patch 済)
- `/etc/firmware/BCM4343A1.hcd` = **armbian の AP6212A 版**
  (SHA `d396912a…`、 33376 bytes)
- `/etc/firmware/BCM4343A1.hcd.rpi` = 旧 RPi 版 (SHA `c096ad4a…`、 30049 bytes、
  rollback 用)
- `/usr/src/` = jcs rk3128 ブランチの shallow clone (.git は削除済)、 build
  artifact `obj/` ごと残存 (再 build 時に再利用可)
- `dmesg | grep bcmbt` → **`bcmbt0: address XX:XX:XX:XX:XX:XX`** ✨
- `dmesg | grep bwfm` → `bwfm0: address XX:XX:XX:XX:XX:XX` (Wi-Fi 健全)
- ssh <your-pomera-user>@<dm250-lan-ip> で母艦 Mac から鍵認証 + doas nopass で操作可能

#### 戻す場合 (Plan 1 化)

BT を完全に切りたい場合 (Wi-Fi 専念モード):

```sh
# on pomera
doas mv /etc/firmware/BCM4343A1.hcd /etc/firmware/BCM4343A1.hcd.bak
doas reboot
# → bcmbt0: failed to load firmware ... と出るだけ、 chip は低電力、 Wi-Fi 影響なし
```

カーネル自体を Plan 2 patch 前に戻す場合:

```sh
# on pomera
doas mv /bsd /bsd.plan2-patched
doas cp /bsd.jcs-original /bsd
doas chmod 700 /bsd
doas reboot
```

RPi 旧 firmware に戻す場合 (失敗状態に戻る):

```sh
# on pomera
doas mv /etc/firmware/BCM4343A1.hcd /etc/firmware/BCM4343A1.hcd.armbian
doas mv /etc/firmware/BCM4343A1.hcd.rpi /etc/firmware/BCM4343A1.hcd
doas reboot
```

### DM250 ハード配線が確定

[03-dm250-bt-pinmap.md](03-dm250-bt-pinmap.md) に全部書いたが要約:

- UART: **uart0** (`0x20060000`)。コンソール (uart1) と別なので占有可
- GPIO: BT_REG_ON=**GPIO3_C5**, BT_WAKE=**GPIO3_D2**, HOST_WAKE=**GPIO3_C6 (active-low)**, BT_UART_RTS=**GPIO0_C1**
- BT chip: BCM43438 互換 (= AP6212A = BCM43430A1)。 firmware: 配布元 `BCM43430A1.hcd` → kernel 期待 `BCM4343A1.hcd` (リネーム必須)

### BT patchram は RPi-Distro/bluez-firmware の `master` ブランチ一択

- linux-firmware には **無い** (BT patchram は再配布条件の都合で取り込まれていない)
- RPi 側 SHA256: `c096ad4a5c3f06ed7d69eba246bf89ada9acba64a5b6f51b1e9c12f99bb1e1a7` (30,049 bytes)
- リポにバイナリ自体は置かない。ビルド時に取得 + SHA 検証

## 採用方針 (この時点で凍結したもの)

- **mux protocol v0 draft 凍結** ([PROTOCOL.md](PROTOCOL.md))。両端でこれを実装する。v1 への昇格は Phase A Step 5 完了時
- **pf divert-to を本命、tun(4) を保険** ([04-outbound-intercept.md](04-outbound-intercept.md))。Phase A Step 6 で実測して降伏判定
- **DNS は DoT (Cloudflare/Quad9)、unbound forward-only** ([etc/unbound.conf.sample](etc/unbound.conf.sample))。Tailscale 起動時の `controlplane.tailscale.com` 解決を mux 経由 TCP で通す

## Android 側の進捗

`../android_app/` を **1-E (Shizuku) ベースから 1-G mux server に全面書き換え済み**。

- 削除: `tether/`, `privileged/`, `aidl/`, Shizuku 依存, ASCII protocol, token
- 追加: `mux/Frame.kt`, `mux/FrameWriter.kt`, `mux/MuxServer.kt`, `mux/TcpStream.kt`, `mux/UdpStream.kt`
- 必要 permission は `BLUETOOTH_*` + `INTERNET` + `FOREGROUND_SERVICE_*` のみ。**signature 系ゼロ**

build 検証はホスト Mac 上 / Android Studio で別途。

## 次にやること (Phase A 着手の最短経路)

1. ~~**`bcmbt_fdt.c` ソース読解**~~ — **2026-05-26 完了**。 上記「bring-up 分岐確定」セクションに結論を記録
2. **USB HCI ドングル調達** — CSR8510 A10 系。amd64 OpenBSD で HCI 到達確認 ([01-hci-bringup.md](01-hci-bringup.md))
3. **BTstack clone + OpenBSD build 試行** — [02-btstack-port-notes.md](02-btstack-port-notes.md) に書いた差分予想を実測で潰す
4. **panctl 雛形作成** — DESIGN.md §4 のディレクトリ構成。`main.c` + `mux_client.c` から。Android 側 PROTOCOL.md と pair で実装すれば echo テストまでは早い
5. **Phase A Step 6 (pf divert) 実機検証** — [04-outbound-intercept.md](04-outbound-intercept.md) の手順をそのまま

## DESIGN.md オープン決定事項への現状回答 (DESIGN.md §8)

| # | 質問 | 現状 |
|---|---|---|
| 1 | Phase A 開発機 (Mac VM か実機か) | **未決**。USB passthrough の手間次第で Mac VM でも可だが、CSR8510 を毎度 detach するなら実機 ThinkPad のほうが楽 |
| 2 | HCI USB ドングル調達 | **未決**。CSR8510 A10 を推奨済み ([01-hci-bringup.md §0](01-hci-bringup.md)) |
| 3 | BT patchram の入手元 | **確定**: RPi `bluez-firmware` master の `BCM43430A1.hcd` (SHA `c096ad4a...`)、 **`/etc/firmware/BCM4343A1.hcd` にリネーム配置** ([etc/firmware.README.md](etc/firmware.README.md)) |
| 4 | panctl ライセンス / 公開先 | **未決** |
| 5 | outbound intercept 方式 | **方針確定**: divert-to 本命、tun(4) 保険。Phase A Step 6 で実測判定 ([04-outbound-intercept.md](04-outbound-intercept.md)) |
| 6 | mux protocol version 番号 | **確定**: v0 / draft で着手、Phase A Step 5 完了時に v1 凍結 ([PROTOCOL.md](PROTOCOL.md)) |
