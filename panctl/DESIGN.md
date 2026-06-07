# btstack/ — Pomera DM250 用 BT スタック設計

pomera DM250 (OpenBSD) を **BT RFCOMM クライアント** にして Android アプリ ([`android_app/`](android_app/)) との間に **TCP/UDP mux チャネル**を張り、Tailscale を含む全アウトバウンドトラフィックをそこに乗せるための、ユーザーランド BT スタック設計メモ。

実装ブランチに移る前の合意ドキュメント。**未確定事項は「要検証」と明記する**。

> **位置付け**: 本設計は [`PLANS.md`](PLANS.md) の **軸 1-G (Android アプリで RFCOMM ↔ TCP/UDP mux)** と **軸 3-E (BlueKitchen BTstack を userland で動かす)** の pomera 側実装。Android 側 mux server の設計は別途 `android_app/` 配下。mux ワイヤ仕様は [`PROTOCOL.md`](PROTOCOL.md) を両端共有の正とする。
>
> **2026-05-26 セッション後の状態**: 本ファイル §1.2 と §3 の「要検証」項目の一部が確定済み。新規ファイル [`03-dm250-bt-pinmap.md`](03-dm250-bt-pinmap.md), [`04-outbound-intercept.md`](04-outbound-intercept.md), [`STATUS.md`](STATUS.md), [`PROTOCOL.md`](PROTOCOL.md), [`etc/firmware.README.md`](etc/firmware.README.md), [`etc/unbound.conf.sample`](etc/unbound.conf.sample) を反映してある。**現在の作業着手点と未決事項は [`STATUS.md`](STATUS.md) を正**とする。

## 1. 大前提

### 1.1 OpenBSD には BT カーネル実装がない

- 6.0 (2016) で `bluetooth(4)`, `btsco(4)`, `ubt(4)`, BNEP, L2CAP, SDP, RFCOMM が**全部削除**された。以降復活していない。
- 親 README と wake_android_tether/README.md は `btpan(4)` を当然のように引用しているが、**現存しない**。前提を改める必要がある。
- カーネルから利用可能なローレベル経路は以下のみ:
  - USB を raw で叩く `ugen(4)`
  - シリアルポート `com(4)` / `uart(4)`
  - GPIO 制御 (`gpio(4)`, ARM 系では FDT 経由)
- したがって BT スタックは **100% ユーザーランドに実装する** しかない。

### 1.2 DM250 のハードウェア

joshua stein の DM250 移植記事から確定:

| 項目 | 値 |
|---|---|
| SoC | Rockchip **RK3128** (ARM Cortex-A7 quad, 32bit) |
| RAM / eMMC | 1 GB / 8 GB |
| 無線モジュール | AMPAK **AP6212A** (= Broadcom **BCM43430** + Cypress 系ファーム) |
| WiFi 結線 | SDIO ← OpenBSD `bwfm(4)` で動作確認済み (要 `brcmfmac43430-sdio` + `nvram_ap6212a.txt`) |
| BT 結線 | **UART0** (`0x20060000`, GIC_SPI 20, base clk 24 MHz, 初期 115200 8N1)。詳細は [`03-dm250-bt-pinmap.md`](03-dm250-bt-pinmap.md) |
| BT ファーム | Broadcom patchram 形式。 配布元 `BCM43430A1.hcd` (RPi-Distro/bluez-firmware master、SHA256 `c096ad4a...`)、 **カーネル期待は `BCM4343A1.hcd`** にリネーム配置 (`/etc/firmware/BCM4343A1.hcd`)。 投入は **カーネル (`bcmbt_fdt.c`) が boot 時に実施**、 userland 側 patchram は不要。 詳細 [`etc/firmware.README.md`](etc/firmware.README.md) |
| BT GPIO | `BT_REG_ON` = **GPIO3_C5** (active-high)、`BT_WAKE` = **GPIO3_D2**、`HOST_WAKE` = **GPIO3_C6** (active-low)、`BT_UART_RTS` = **GPIO0_C1** (patchram 中の override 用)。詳細 [`03-dm250-bt-pinmap.md`](03-dm250-bt-pinmap.md) |

> **重要発見 (2026-05-26)**: jcs/openbsd-src の rk3128 ブランチに **`sys/dev/fdt/bcmbt_fdt.c`** という BT bring-up カーネルドライバが既に存在する。 `compatible = "brcm,bcm43438-bt"` を直接 consume する。 ソース読解の結果（[`STATUS.md`](STATUS.md) §「bring-up 分岐確定」）、 カーネルは **GPIO + UART 初期化 + HCI Reset + Download Minidriver + patchram (`/etc/firmware/BCM4343A1.hcd`) 投入 + post-firmware reset + BD_ADDR 読み出し** までを boot 時に一気に終わらせ、 以後は何もしない (`DV_DULL`、 cdev なし)。 親 `com(4)` が引き続き `/dev/cua00` を提供するので、 **userland は標準の TTY で 115200 8N1 の HCI ready な BT chip にアクセスする**形になる。 結果として Phase B の userland 側 patchram / baud 切替は **不要** (DESIGN §3.B Step 4 から削除)、 transport は予定通り `port/posix-h4`。

参考リポジトリ (UART/GPIO 配線特定の一次情報):
- `https://github.com/jcs/openbsd-src` (rk3128 ブランチ): DTB と `sys/dev/fdt/bcmbt_fdt.c`
- `https://github.com/jcs/linux-dm250`: Linux 側の dts (BT ノード抜粋は [`03-dm250-bt-pinmap.md`](03-dm250-bt-pinmap.md) §4)

### 1.3 採択するユーザーランド BT スタック

[**BlueKitchen BTstack**](https://github.com/bluekitchen/btstack) を採用する。

- ライセンス: BSD-3-Clause + 商用利用条項あり (個人/オープン用途は問題なし)
- POSIX ターゲットあり (`port/libusb`, `port/posix-h4`, `port/posix-h5`)
- **Broadcom BCM 系ファームの patchram アップロード機能を内蔵** (`chipset/bcm/`) — AP6212A にも理論上使えるが、 DM250 ではカーネルが patchram を投入するので **userland 側で chipset/bcm を使う必要はない**（Phase A の amd64 + 一部 BCM 系 USB ドングルでのみ意味がある）
- **RFCOMM (SPP) のクライアント例** (`example/spp_counter.c` ほか) があり、本設計はこれをひな型とする

選定理由:
- BlueZ や NetBSD BT は「カーネル統合前提」で OpenBSD ユーザーランドへの移植コストが極端に高い。
- BTstack は最初から "userspace stack, talks HCI directly to controller" が設計思想で、移植コストが最小。
- BCM patchram を**スタック側が知っている**のは AP6212A 案件で決定的な利点。

### 1.4 スコープ: RFCOMM client のみ

PLANS.md 第 2 改訂で **軸 1-G (App 内 RFCOMM ↔ TCP/UDP mux)** が採用されたことで、pomera 側 BT スタックの射程は以下に縮退した:

| 機能 | 必要性 |
|---|---|
| HCI transport (libusb / UART H4) | ✓ 必須 |
| L2CAP | ✓ 必須 (RFCOMM の下層) |
| SDP browse | ✓ 必須 (Android アプリの RFCOMM チャネル探索) |
| SSP pairing | ✓ 必須 (初回ペアリング) |
| RFCOMM client | ✓ **これだけ** |
| BNEP | ✗ 不要 (退避案 1-H に降りた場合のみ復活) |
| PAN PANU / NAP | ✗ 不要 (同上) |
| `tap(4)` / `tun(4)` ブリッジ | ✗ 不要 |
| `dhclient` 統合 | ✗ 不要 (mux 経由なので IP 設定不要) |
| A2DP / HFP / HID / GATT | ✗ 不要 |

結果として実装ボリュームが旧設計 (PANU + BNEP + tap ブリッジ) より明確に小さい。

---

## 2. 目標構成

```
[pomera DM250 / OpenBSD]
   ┌──────────────────────────────────────────────────────────────┐
   │  panctl  (本ディレクトリで開発する常駐プロセス)                │
   │                                                              │
   │  ┌────────────────────────────────────────────────────────┐  │
   │  │  BTstack (静的 link)                                    │  │
   │  │   ├─ HCI transport (差し替え可能)                       │  │
   │  │   │   ├─ libusb backend   ← Phase A (USB HCI dongle)    │  │
   │  │   │   └─ POSIX H4 (TTY)   ← Phase B (DM250 /dev/cua00)  │  │
   │  │   ├─ (patchram は kernel `bcmbt_fdt.c` が boot 時に実施) │  │
   │  │   ├─ L2CAP / SDP / SSP                                  │  │
   │  │   └─ RFCOMM client (spp_counter ベース)                  │  │
   │  └────────────────────────────────────────────────────────┘  │
   │  ┌────────────────────────────────────────────────────────┐  │
   │  │  mux client                                             │  │
   │  │   ├─ pf divert socket (TCP+UDP outbound intercept)      │  │
   │  │   ├─ stream/datagram framing over single RFCOMM stream  │  │
   │  │   └─ per-flow state (TCP stream_id, UDP NAT timeout)    │  │
   │  └────────────────────────────────────────────────────────┘  │
   └────────────────────────────┬─────────────────────────────────┘
                                │
            ┌───────────────────┴─────────────────────┐
            │ Phase A: ugen(4) 経由で USB HCI ドングル │
            │ Phase B: /dev/cua?? 経由で AP6212A (UART) + GPIO bring-up │
            └───────────────────┬─────────────────────┘
                                │ RF (Bluetooth RFCOMM channel)
                                ▼
                  [Android: 自作アプリの RFCOMM サーバ]
                                │
                                │ mux protocol decode
                                ▼
                  [Android: per-flow TCP/UDP socket on cell network]
                                │
                                ▼
                            キャリア回線
                                │
            ┌───────────────────┴───────────────┐
            │ Tailscale (pomera 上で動作)         │
            │   - WireGuard UDP は mux 経由で出る │
            │   - DERP fallback は mux 経由 TCP   │
            └───────────────────┬───────────────┘
                                ▼
                          自宅 PC tmux
```

**ポイント**:
- pomera から見ると、mux client が `tun0` 相当の「出口」になる。Tailscale は通常の OpenBSD 環境と同じく `tailscaled` を走らせるだけ
- `pf` の `divert-packet` / `divert-to` で全アウトバウンド TCP/UDP を mux client に流すか、もしくは **`tun(4)` を NAT 装置として被せて mux client が `tun0` を read/write する** 形になる。どちらが綺麗かは Phase A Step 5 で実機検証
- Android 側は signature 権限ゼロで動く foreground service。詳細は `android_app/` 配下 (1-G 用に書き換え予定)

## 3. フェーズ分け

「**ハードウェア依存を最後にする**」原則で 2 フェーズに切る。BTstack のスタック側を amd64 + 安いドングルで先に固めれば、DM250 実機作業は **HCI transport の差し替えと GPIO/UART bring-up に縮退** する。

### Phase A — amd64 OpenBSD + USB HCI ドングルで RFCOMM mux を完成させる

開発 PC (Mac) 上の OpenBSD VM、または手元の amd64 マシンで実施。

ステップ:

1. **HCI 到達確認** — `pkg_add libusb` → 標準 HCI ドングル (CSR8510 A10 推奨。安価で実績豊富) を挿し、`ugen` attach を確認。生 HCI で `HCI_Reset` が返ることを libusb の小プログラムで確認。手順は [01-hci-bringup.md](01-hci-bringup.md) に切り出した。
2. **BTstack `port/libusb` の OpenBSD ビルド** — gmake で素直に通るかを確認。詰まりそうな点は [02-btstack-port-notes.md](02-btstack-port-notes.md) にまとめた。`hci_dump`, `inquiry`, `spp_counter` の example が動けば BTstack 全体 OK。
3. **Android とのペアリング** — SSP / Just Works で片方向ペアリング。鍵を BTstack の TLV (`btstack_tlv_posix.c`) に永続化。
4. **mux プロトコル仕様策定** — **完了 (v0 / draft)**: [`PROTOCOL.md`](PROTOCOL.md) に仕様凍結。6 バイトヘッダ (ver/type/stream_id/length)、HELLO/BYE/PING/PONG/TCP_*/UDP_* の frame type、credit-based flow control、再接続セマンティクス。Phase A Step 5 完了時に v1 に昇格して固定
5. **RFCOMM client + mux client の実装** — BTstack の `spp_streamer_client.c` をベースに RFCOMM チャネルを張る。フレーム送受信ループ内で PROTOCOL.md の encode/decode。Android 側 mux server (`android_app/`) と HELLO 交換 → TCP_OPEN echo → UDP_PACKET echo まで通す
6. **pomera 側 outbound intercept** — **方針確定**: pf `divert-to` 本命、`tun(4)` 保険。実機検証手順は [`04-outbound-intercept.md`](04-outbound-intercept.md) の通り。Phase A Step 6 で実測して降伏判定
7. **Tailscale 起動試験** — `pkg_add tailscale` → `tailscaled` 起動 → DNS bootstrap (DoT 経由、`unbound` 設定は [`etc/unbound.conf.sample`](etc/unbound.conf.sample)) で `controlplane.tailscale.com` 解決 → WireGuard UDP が mux を抜けてピアに届くか確認。直接 UDP path が成立すれば Tailscale はフルスピード、ダメなら DERP fallback (TCP) でとりあえず疎通
8. **rc.d 化** — `/etc/rc.d/panctl` で起動・再接続・鍵永続化

Phase A 完了基準:
- amd64 OpenBSD で `ssh home.tailnet.ts.net` が **Tailscale 経由で** 成立 (mux RFCOMM 経由)
- 切断 → 自動再接続が動く

### Phase B — DM250 実機への移植

ハードウェア依存を一気に詰める。 ソース読解の結果 (詳細 [`STATUS.md`](STATUS.md) §「bring-up 分岐確定」)、 `bcmbt_fdt.c` が **GPIO + UART 初期化 + HCI Reset + Download Minidriver + patchram + post-reset + BD_ADDR 読み出し** までを kernel 側で済ませることが分かったので、 userland 側のスコープは「すでに HCI ready な `/dev/cua00` を開いて mux を流すだけ」に縮退した。

1. **UART デバイス名確認** — **確定**: `uart0` (`0x20060000`) ＋ 親 `com(4)` 経由で **`/dev/cua00`**。 実機 `dmesg | grep bcmbt` で `bcmbt0 at com0: address xx:xx:...` が出ることを確認 (BD_ADDR 出力が成功フラグ)
2. **GPIO bring-up** — **不要**。 BT_REG_ON / BT_WAKE / HOST_WAKE / BT_UART_RTS の全部を `bcmbt_fdt.c` が触る (`shutdown-gpios` / `uart-rts-gpios` / `device-wakeup-gpios` の DT プロパティ経由)。 詳細 [03-dm250-bt-pinmap.md](03-dm250-bt-pinmap.md) §5
3. **ファーム配置** — `BCM43430A1.hcd` を RPi-Distro/bluez-firmware master から SHA256 検証付きで取得し、 **`/etc/firmware/BCM4343A1.hcd` にリネーム配置**（カーネル `loadfirmware(9)` 期待名）。 詳細 [`etc/firmware.README.md`](etc/firmware.README.md)。 これは openBSD 側 `install.md §6.2` のステップに切り出し済み
4. **HCI transport の差し替え** — コンパイル時設定で `port/libusb` → `port/posix-h4`、 chipset は `chipset/none`（`btstack_chipset_bcm_*` は呼ばない）。 cua00 を 115200 8N1 + CRTSCTS で open(2)。 **patchram も baud 切替も userland では実施しない**（kernel 完了済み）
5. **電源管理** — `bcmbt_fdt.c` が `device-wakeup-gpios` を常時 ON にする。 sleep フローは現状未対応。 最初は気にせず通す
6. **エンドツーエンド回帰** — Phase A の `ssh home.tailnet.ts.net` 試験を DM250 上で再現

Phase B 完了基準: DM250 上で `tmux attach -t claude-master` が Tailscale + mux 経由で動く

---

## 4. ディレクトリ構成案 (このディレクトリ配下)

```
btstack/
├── DESIGN.md                ← 本ファイル
├── 01-hci-bringup.md        ← Phase A Step 1 の手順
├── 02-btstack-port-notes.md ← BTstack を OpenBSD で build する際の予想差分
├── third_party/             ← BTstack を submodule か unpacked tarball で
├── panctl/
│   ├── main.c              ← BTstack run loop + RFCOMM 接続管理
│   ├── mux_client.c        ← フレーミング、stream/datagram 多重化
│   ├── divert.c            ← pf divert socket or tun(4) glue
│   ├── transport_libusb.c  ← Phase A 用 HCI transport (BTstack の port/libusb 流用)
│   └── transport_h4.c      ← Phase B 用 (BTstack の port/posix-h4 のみ。 patchram は kernel が boot 時に実施)
├── etc/
│   ├── rc.d.panctl         ← /etc/rc.d/panctl にインストール
│   ├── unbound.conf.sample ← DoT 設定例 (Tailscale bootstrap DNS 用)
│   └── firmware.README     ← BT patchram の入手手順 (バイナリ自体は置かない、 配置先は openBSD/install.md §6.2)
└── tools/
    ├── bt_reg_on.c         ← GPIO で BT_REG_ON 制御する単機能バイナリ
    └── hci_smoke.c         ← libusb 経由で HCI_Reset を投げるだけの疎通確認
```

このレイアウトは確定ではない。Phase A 着手時にもう一度見直す。

## 5. 主なリスク

| リスク | 兆候 | 対処 |
|---|---|---|
| **BTstack の OpenBSD port が無い** | `port/libusb` の Makefile が Linux 前提のフラグを使っている、`select(2)` 周りの差で warning が大量に出る | gmake で `posix-*` 系のソースを直接 compile する build を panctl 側に持つ。BTstack を install せず static link |
| **`ugen(4)` での async transfer 詰まり** | BT ACL データの取りこぼし、HCI イベント遅延 | `ugen(4)` のバッファサイズチューニング (`USB_SET_*`)、必要なら kernel パッチ |
| ~~**OpenBSD termios で BCM 用高速 baud (3 Mbps) が出ない**~~ | — | **解消**: kernel が baud 切替しない設計（115200 固定）。 SSH/tmux 用途では実効十分 |
| **AP6212A の patchram 入手難 / バージョン差** | `bcmbt0: failed to load firmware BCM4343A1.hcd` または attach 後 BD_ADDR 表示なし | RPi-Distro `bluez-firmware/broadcom/BCM43430A1.hcd` を使い、 **`/etc/firmware/BCM4343A1.hcd` にリネーム配置**。 SHA を README に記録 |
| **Android 側 SSP 要件** | ペアリングが LE Secure Connections 要求して落ちる | BTstack の Pairing config を Secure Connections 有効、Numeric Comparison 対応に。最近の Android 12+ で要 LE SC |
| **DM250 BT_REG_ON の GPIO 番号がわからない** | bring-up で chip が応答しない | `jcs/linux-dm250` の dts を読む、最悪は分解して PCB トレース |
| **pf divert で全 outbound intercept が安定しない** | 一部 socket が divert を通らない / Tailscale の WireGuard UDP が出口を見つけられない | `tun(4)` をデフォルトルートにして mux client が `tun0` を read/write する方式に切替。1-H に降りる手前の中間構成 |
| **mux protocol の flow control が雑で stall** | 大きい TCP 転送で固まる | 各 stream にウィンドウサイズ概念を入れる (yamux 由来の credit-based)。最初は固定バッファで動かして実測して入れる |
| **Tailscale UDP fast path が mux 上で遅い** | direct UDP は動くがピア間 RTT が高い | 当面は DERP fallback (TCP) 経由でも SSH+tmux なら問題なし。実測ベースで判断 |
| **帯域** | SSH/tmux は 50 kbps もあれば足りるので非問題。`scp` 系の大きい転送はそもそも厳しい | 大容量は USB テザリング (PLANS.md 2-C) で別途 |

## 6. 関連 README との整合

このディレクトリを進める過程で、以下の記述は事実関係を直す必要がある:

- 親 `pomera-workspace/README.md` の「BT PAN (`btpan(4)`)」記述 → **OpenBSD 6.0 以降 BT スタックは削除されており、`btpan(4)` は存在しない。本ディレクトリで userland 実装 (BT PAN ではなく RFCOMM mux) を作る** に書き換える
- `wake_android_tether/README.md` の比較表「BT PAN ◎」 → 修正必要。「BT PAN」ではなく「BT RFCOMM mux」が本命であることを反映
- `wake_android_tether/CLAUDE.md` の優先順 (BT PAN → USB tether → Instant Hotspot) は方針として有効。本設計は **PAN を 1-H 退避案として残しつつ、本命を RFCOMM mux (1-G) に振った形**

修正は Phase A の最初のマイルストーン (HCI 到達確認) が済んだタイミングでまとめてかける。

## 7. やらないこと (このディレクトリの境界)

- **BNEP / PANU / PAN NAP**: 退避案 1-H に降りた場合のみ復活。1-G 採用時は実装しない
- **`tap(4)` / `tun(4)` Ethernet モードでの BNEP frame ブリッジ**: 同上
- **A2DP / HFP / HID / GATT など RFCOMM 以外のプロファイル**: 必要なら別ディレクトリで
- **DM250 内蔵 WiFi のスタック追加実装**: `bwfm(4)` で既に動くので射程外
- **Samsung Instant Hotspot の逆解析**: 親 wake_android_tether/CLAUDE.md の通り後回し
- **BT スタック自体の新規実装**: BTstack を活用する。車輪の再発明はしない
- **Android アプリの mux server 実装**: `android_app/` 配下の責務

## 8. オープンな決定事項

現状の正は [`STATUS.md`](STATUS.md) §「DESIGN.md オープン決定事項への現状回答」表。要約:

| # | 質問 | 状態 |
|---|---|---|
| 1 | Phase A の amd64 開発機 (Mac VM / 実機) | **未決** (USB passthrough の手間次第) |
| 2 | HCI USB ドングル調達 | **未決** (CSR8510 A10 推奨済み、[`01-hci-bringup.md`](01-hci-bringup.md) §0) |
| 3 | BT patchram の入手元 | **確定**: RPi `bluez-firmware` master の `BCM43430A1.hcd` (SHA `c096ad4a...`)、 **`/etc/firmware/BCM4343A1.hcd` にリネーム配置** ([`etc/firmware.README.md`](etc/firmware.README.md)) |
| 4 | panctl ライセンス / 公開先 | **未決** |
| 5 | outbound intercept 方式 | **方針確定**: divert-to 本命、tun(4) 保険 ([`04-outbound-intercept.md`](04-outbound-intercept.md))。Phase A Step 6 で実測判定 |
| 6 | mux protocol version | **確定**: v0 / draft、Phase A Step 5 完了時に v1 凍結 ([`PROTOCOL.md`](PROTOCOL.md)) |
| 7 | bcmbt_fdt.c の機能範囲 | **確定 (2026-05-26)**: GPIO + UART init + patchram + post-reset + BD_ADDR 読み出しを boot 時に完了。 cdev なし (`DV_DULL`)、 userland は親 `com(4)` の `/dev/cua00` でアクセス。 詳細 [`STATUS.md`](STATUS.md) §「bring-up 分岐確定」 |
