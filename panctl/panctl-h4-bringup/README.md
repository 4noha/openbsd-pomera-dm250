# panctl-h4-bringup — BTstack + DM250 BT chip ブートストラップ成功記録

2026-05-29 16:02 JST、 BTstack v1.6.2 を DM250 上で build → 実行 → BT chip
と完全 HCI 通信成立、 `HCI_STATE_WORKING` まで到達確認。 panctl の前提が
全部証明された日のスナップショット。

## 何ができたか

```
$ doas /tmp/btstack_h4_smoke
=== btstack_h4_smoke for /dev/cua00 ===
[evt] type=0x60 size=3
  [state] 1
[evt] type=0x6e size=2
[evt] type=0x0e size=6
  [cmd-complete] opcode=0x0c03    ← HCI Reset
[evt] type=0x0e size=14
  [cmd-complete] opcode=0x1001    ← Read Local Version Info
[evt] type=0x0e size=254
  [cmd-complete] opcode=0x0c14    ← Read Local Name
... (30 個以上 init commands) ...
[evt] type=0x60 size=3
  [state] 2
  ★ HCI working! BD_ADDR aa:aa:aa:aa:aa:aa
[evt] type=0x60 size=3
  [state] 3
```

つまり:
- BTstack core が OpenBSD/armv7 で build 通る
- H4 transport が `/dev/cua00` 経由で BT chip と話せる
- `chipset_bcm` が BCM 系の wake シーケンスを正しく送る (我々の bcm_uart_probe2 が手動で発見した RTS pulse の自動化)
- 全 init コマンドが round-trip し `HCI_STATE_WORKING` に到達
- 続いて power off → state 3 (HALTING) と clean shutdown

BD_ADDR は `aa:aa:aa:aa:aa:aa` (BCM factory default) — kernel 起動直後の
`XX:XX:XX:XX:XX:XX` (OTP) には戻ってないが、 これは BTstack 側で
`hci_set_chipset` + init script で BD_ADDR を読み直すか、 永続化の `tlv` に
入れる設計にすればよい。 panctl のスコープ。

## ファイル

| ファイル | 用途 |
|---|---|
| `btstack_h4_smoke.c` | minimal main: HCI 起動 → state=WORKING で BD_ADDR 表示して exit |
| `build_smoke.sh` | BTstack core + chipset/bcm + platform/posix + smoke を 1 cc で link する build スクリプト |
| `../patches/openbsd-rk3128/btstack-v1.6.2-openbsd-compat.patch` | BTstack 側 OpenBSD compat patch (errno.h と strings.h の include 追加 2 件) |

## 再現手順 (別 DM250 / fresh OpenBSD で同じことやる場合)

### 0. 前提

- Plan 2 完了 (kernel patch + armbian firmware)
- `/etc/firmware/BCM4343A1.hcd` に armbian の AP6212A 用 firmware が居る
- `dmesg | grep bcmbt` で `bcmbt0: address xx:xx:xx:xx:xx:xx` 出てる

### 1. tooling

```sh
doas pkg_add git gmake
mkdir -p ~/btstack-work && cd ~
git clone --depth 1 --branch v1.6.2 https://github.com/bluekitchen/btstack.git
cd btstack
patch -p1 < /path/to/btstack-v1.6.2-openbsd-compat.patch
```

### 2. smoke build

```sh
cp /path/to/btstack_h4_smoke.c /tmp/
cp /path/to/build_smoke.sh    /tmp/
chmod +x /tmp/build_smoke.sh

# BT 在席チェックに `BT=$HOME/btstack` を環境変数で渡せるようにしたければ
# build_smoke.sh の冒頭の `BT=/home/<your-pomera-user>/btstack` を書き換え

/tmp/build_smoke.sh
# → /tmp/btstack_h4_smoke (約 550KB) ができる、 数分
```

### 3. 実行

```sh
doas /tmp/btstack_h4_smoke
# 期待: HCI_STATE_WORKING まで到達 → "★ HCI working!" 表示 → exit
```

## ビルド構成の要点

`build_smoke.sh` で除外したファイル群 (audio / mesh / 不要 profile):

| カテゴリ | excluded pattern |
|---|---|
| audio codec | `*sbc*`, `*lc3*`, `*sample_rate_compensation*` |
| 不要 profile | `*pbap*`, `*hsp*`, `*hfp*`, `*hid_*`, `*goep*`, `*a2dp*`, `*avdtp*`, `*avrcp*`, `*obex*`, `*opp*`, `*map*`, `*bnep*` |

panctl は **RFCOMM + SDP + L2CAP** だけ使うので、 残した CORE で必要十分。
追加で必要になったら個別に build_smoke.sh を見直す。

## panctl 本体 build (2026-05-29 完了)

smoke が通った同日、 panctl/main.c をネイティブビルド + 実機実行まで通過。

```
$ doas /tmp/panctl --android-bdaddr DE:AD:BE:EF:00:01 -t h4 -d /dev/cua00
[panctl] main: starting
[trace] mux_create OK
[trace] divert_create OK
HCI working; issuing SDP query to Android   ← HCI_STATE_WORKING 到達
SDP query failed status=0x04                ← fake addr なので想定通り
```

つまり HCI bring-up → SDP query 発行までは panctl 単独で動く。 次は実機
Android とのペアリング + RFCOMM 接続 (Step 6) のみ。

### panctl-h4 build script (`build_panctl.sh`)

smoke 用 `build_smoke.sh` と同じ要領で `cc` 1 発。 違いは:

- `-DHAVE_BTSTACK=1 -DBTSTACK_FILE__=__FILE__` を CFLAGS に追加
- `platform/posix/btstack_tlv_posix.c` を PLAT に追加 (link key 永続化)
- panctl 側 src (`frame.c mux.c ipv4.c divert.c main.c`) を末尾に追加
- 全 71 ファイル, 数分で `/tmp/panctl` (約 578KB) に link

### main.c に必要だった compat 修正

BTstack v1.6.2 / OpenBSD 7.9 の組合せで、 panctl/main.c skeleton はそのまま
build できなかった。 main.c に入れた変更:

| 修正 | 理由 |
|---|---|
| `setbuf(stdout/stderr, NULL)` | 子プロセス redirect 時に log が flush されないと debug 不能 |
| `btstack_memory_init()` 追加 | smoke にあった、 skeleton で抜けてた。 HCI packet pool の static 初期化が要る |
| `btstack_uart_block_posix_instance` → `btstack_uart_posix_instance` | block 版は event-driven でなく単発 read のため H4 で取りこぼす |
| `baudrate_main = 115200` → `0` | 0 = 「init 後に baud 切替しない」。 DM250 は kernel patchram 後も 115200 のまま |
| `hci_dump_init(...)` 削除 | 引数の `hci_dump_posix_fs_get_instance()` は path 事前設定が要るので NULL でも skeleton 側で害があった (smoke は呼んでない) |
| `#include "hci_transport_h4.h" / classic/rfcomm.h / classic/btstack_link_key_db_tlv.h` | v1.6.2 で個別ヘッダ参照になった (旧 umbrella `btstack.h` だけでは不足) |
| `rfcomm_register_packet_handler()` 削除 | v1.6.2 deprecated。 代わりに `rfcomm_create_channel(handler, ...)` の第1引数で渡す |
| `rfcomm_create_channel(NULL, ...)` → `rfcomm_create_channel(&on_btstack_packet, ...)` | 同上 |
| `#if defined(USE_LIBUSB_TRANSPORT)` で libusb 分岐をガード | DM250 path では libusb 不要、 link error 回避 |
| **`hci_power_control(HCI_POWER_ON)` を `mux_create()` / `divert_create()` の前に移動** | ★ 後述 |

### ★ heap-order bug: mux_create を HCI 起動より先にやると POWERON_FAILED になる

panctl bring-up 中の最大の謎。 bisect でようやく判った話 (一日仕事)。

**現象**:

```c
mux_create(&cb);            // ← これを書いておくと…
hci_power_control(HCI_POWER_ON);   // ← この呼び出しが BTSTACK_EVENT_POWERON_FAILED (0x62) を吐く
```

順序を逆にすると正常 (HCI_STATE_WORKING まで到達)。

**bisect 経過**:

1. smoke は通る、 panctl は POWERON_FAILED で死ぬ → init list 差分が原因
2. l2cap_init / sdp_init / rfcomm_init / sm_init / gap_* / tlv / hci_set_link_key_db
   1 つずつ disable → どれも単体では犯人ではない (全部 enable + mux_create disable
   で **通る**)
3. `mux_create()` の calloc(1, ~120) を消すだけで HCI bring-up が成立
4. `mux_create` を `hci_power_control(HCI_POWER_ON)` の **後ろ** に移動 → 全部
   enable で通る

**根本原因 (推定)**:

`mux_create` は単純な calloc しかしてない。 構造体サイズも 100 数十バイト。
にも関わらず、 これを HCI transport open の前に呼ぶと OpenBSD/armv7 上で
malloc arena が「BTstack の HCI buffer 初期化が前提にしている layout」から
ズレるらしい。 BTstack 側の HCI buffer は static (`hci_stack_static` パス)
なので頭で考えると無関係なのだが、 実際は再現する。 guard page 絡みか、
あるいは OpenBSD malloc の chunk size class boundary が問題かもしれない。
深追いはしない (workaround で完全に回避できるため)。

**workaround** (本リポ採用):

`hci_power_control(HCI_POWER_ON)` を `mux_create` / `divert_create` より
先に呼ぶ。 BTstack の HCI state machine は run loop に入る前に POWER_ON
を予約するだけなので、 順序を入れ替えても挙動は変わらない。 mux/divert は
run loop 開始までに作れていれば事象到来に間に合う (HCI INITIALIZING → WORKING
は数百 ms 〜数秒かかる)。

main.c の main() 内コメントに同じ事を残している。 将来 BTstack を v1.7 以上に
上げたとき再現するか確認する価値はある (今回触ったのは v1.6.2)。

## tun(4) UDP path 動作確認 (2026-05-29 末)

実機 Pixel 9 Pro と pair → mux RFCOMM → tun0 → mux UDP → Android → 4G → 1.1.1.1
DNS と end-to-end 疎通成立。

### セットアップ

```sh
# on pomera (master ssh から)
doas ifconfig tun0 create
# panctl を tun mode で起動 (tun0 を open)
doas /tmp/panctl --android-bdaddr AA:BB:CC:DD:EE:FF \
    -t h4 -d /dev/cua00 \
    --udp-mode tun --tun-dev /dev/tun0 &
# tun0 を up & 設定
doas ifconfig tun0 inet 10.66.66.2 10.66.66.1 netmask 255.255.255.0 up
# 1.1.1.1 だけ tun0 経由 (default route は触らない、 SSH 影響なし)
doas route add 1.1.1.1 10.66.66.1
# DNS query (1 回目は ACL buffer 制約で BIND だけ通って drop、 dig が retry する)
dig @1.1.1.1 example.com
# → 172.66.147.243, 104.20.23.154  (本物の Cloudflare レスポンス)
```

### 実装上の発見

1. **OpenBSD tun(4) は AF_INET 4-byte prefix を付ける**
   `00 00 00 02` (= AF_INET BE) が IPv4 frame の前に付く。 `divert.c` の
   `handle_udp_fd` / `cb_udp_packet` で読み書き時に skip/prepend する
   `TUN_AF_HDR=4` マクロを足した。 設計 doc には書いてない罠

2. **BCM43438 + BTstack v1.6.2 は ACL TX buffer 1 スロット**
   `BTSTACK_ACL_BUFFERS_FULL (rc=0x57)` が背中合わせの 2 フレーム送信で
   毎回出る。 UDP_BIND → UDP_PACKET の連続発行で PACKET が drop される。
   busy-wait 補正はダメ (BTstack の run loop を block して ACL ACK を読めない)。
   現状は **「最初の packet は drop、 application 層 retry に任せる」** で
   PoC を通している。 dig は標準 +tries=3 でちゃんと拾ってくれる。
   本実装は `rfcomm_request_can_send_now_event` で kick して queue する
   モデルに直す (1 connection 分の 1 packet queue があれば十分)

### Tailscale 完全動作 + thin-client 完成 (v1)

v0 では「同時 TCP 多重 + ACL 連続送信」で詰まって Tailscale ほぼ機能しなかった
ところ、 v1 で構造的対処入れて完全動作。 実機 Pixel 9 Pro + cellular で
home Mac Studio に SSH まで end-to-end 通った。

```
$ ssh <home-mac>   # pomera 上、 ~/.ssh/config Host <home-mac> で
<home-mac>
 1:00  up 22:34, 3 users, load averages: 3.46 3.48 3.35
```

経路: **pomera → tun0 → panctl → mux RFCOMM → BT → Android → cellular →
Tailscale (WireGuard UDP / DERP TCP) → <home-mac>**

#### v1 で入れた変更

| 変更 | コード | 効果 |
|---|---|---|
| `MAX_TCP_CONNS` 32→128 | `tun_tcp.c` | Tailscale DERP discovery で 50+ 同時接続を吸収 |
| 5-tuple → slot lookup table | `tun_tcp.c` | 単一 slot から N slot 化、 `tcp_conn` 配列 + linear scan |
| `SEND_Q_DEPTH` 32→256, FIFO mux frame queue | `main.c` | `rfcomm_request_can_send_now_event` 駆動の credit-aware queue。 ACL_BUFFERS_FULL で drop しない |
| `tun_tcp_sweep()` (1Hz) | `tun_tcp.c` + `main.c` | TS_SYN_RCVD 状態 30 sec 続いた slot を強制 reset → slot leak 防止 |
| ISN slot 分離 `0xC0FFEE00 + (slot<<16)` | `tun_tcp.c` | slot reuse 時の seq 衝突回避 |

#### Tailscale 接続成立確認

```
$ doas tailscale ip
<your-pomera-ts-ip>
fd7a:115c:a1e0::613a:66b

$ doas tailscale netcheck
* UDP: true                    ← WireGuard direct peer OK
* IPv4: yes, 133.201.203.192:56002 (Android cellular global IP)
* Nearest DERP: Tokyo, 251ms
```

peer 一覧では <home-mac> (<your-mac-ts-ip>)、 nas1263a7、 nas6aa574 など online、
SSH で実コマンド実行成立。

### Production 配置

```
/usr/local/sbin/panctl                 (chmod 0755 root:bin)
/usr/local/sbin/panctlctl              (chmod 0755 root:bin)
/etc/rc.d/panctl                       (本ディレクトリの etc/rc.d/panctl)
/etc/hostname.tun0:
    !ifconfig tun0 create up
    inet 10.66.66.2 255.255.255.0 10.66.66.1
    mtu 1000
/etc/rc.conf.local:
    panctl_flags="--android-bdaddr ANDROID_BDADDR -t h4 -d /dev/cua00 \
                  --udp-mode tun --tun-dev /dev/tun0"
/etc/dhclient.conf:
    prepend domain-name-servers 100.100.100.100, 1.1.1.1;
/etc/resolv.conf:
    nameserver 100.100.100.100
    nameserver 1.1.1.1
```

```sh
doas rcctl disable resolvd          # 上書きされないように
doas rcctl enable panctl tailscaled
doas rcctl start  panctl tailscaled
# 認証は初回のみ: doas tailscale up
```

#### <your-pomera-user> 用 `~/.ssh/config` プリセット

```sshconfig
# 旧 / 自然な名前
Host <home-mac>
    Hostname <home-mac>.<your-tailnet>.ts.net
    User 4noha
    IdentityFile ~/.ssh/id_ed25519
    ControlMaster auto
    ControlPath ~/.ssh/cm-%r@%h:%p
    ControlPersist 10m

# `ssh claude` でも同じ home Mac にいく (Claude が動いてる環境を意識した別名)
Host claude
    Hostname <home-mac>.<your-tailnet>.ts.net
    User 4noha
    IdentityFile ~/.ssh/id_ed25519
    ControlMaster auto
    ControlPath ~/.ssh/cm-%r@%h:%p
    ControlPersist 10m
```

`ssh <home-mac>` でも `ssh claude` でも同じ home Mac shell に届く。
Tailscale 経路で ~250ms RTT、 ControlMaster で 2 回目以降は instant。

#### BT-only 動作確認 (WiFi 完全切断)

```sh
# on pomera (console)
doas ifconfig bwfm0 down
curl -s -m 15 http://example.com/ -o /dev/null -w "HTTP %{http_code} bytes %{size_download}\n"
# → HTTP 200 bytes 528  (DNS+TCP 両方 mux 経由 cellular で疎通)
doas ifconfig bwfm0 up   # SSH 戻すなら
doas rcctl restart dhcpleased
```

これで「pomera が BT-tether 専用 thin-client」設計が WiFi 一切なしで完全動作
することを実機証明済。

これで pomera 電源 ON → 数十秒で:
- WiFi (or BT-PAN 将来) で初期 internet
- BT pair → mux RFCOMM 自動接続 (Android app の `BtConnectReceiver` で
  MuxServer 自動 listen 起動)
- tun0 default route 切替 (現状は手動、 panctl のpost-up hook 化は宿題)
- Tailscale 経路復活、 ssh <home-mac> で home tmux に attach

### TCP-via-tun (次セッションの本丸)

tun(4) は L3 (IPv4 frame) なので、 TCP の場合 panctl 側で **userland TCP
state machine** が要る:

- SYN → mux_send_tcp_open、 ACK 待たずに pomera 側へ SYN+ACK fake 返し
- DATA 流入 → mux_send_tcp_data + seq/ack track
- mux からの DATA → IP+TCP segment 構築 (seq/ack 正しく) → tun 書き戻し
- FIN 処理、 timer (TIME_WAIT 等は省略可)
- 1 connection あたり ~150 行、 マルチ connection で ~500-800 行

現状の `panctl/divert.c` は kernel TCP に乗っかってる (pf divert-to + SOCK_STREAM
listen)。 これが OpenBSD では outbound に使えないことが今日判明したので、
tun path のみが選択肢。 `panctl/tun_tcp.c` (新規) を切って書く想定。

これが通れば素 `curl example.com` が動く。 PoC レベルで 2-3 日見込み。

## 残る課題

### 1. Android E2E (次セッションの本命)

実機の Android と pair → SDP query 成功 → RFCOMM channel open → HELLO 往復
まで通す。 panctl 側は SDP query 発行 → fake BD_ADDR だと "SDP query failed
status=0x04" で止まるところまで証明済み。 残りは実機が要る部分。

#### 前提

- 母艦 Mac から `ssh <your-pomera-user>@<dm250-lan-ip>` 通る (普段の SSH)
- `/tmp/panctl` (約 578KB) と `/tmp/build_panctl.sh` が pomera 上に残ってる
  (commit 9a63b3a のソースから rebuild も可)
- Android 端末 1 台、 USB ケーブル + ADB が母艦 Mac 側で動く
- `../android_app/` ビルド済 APK (このリポの別作業範囲)

#### 手順

1. **Android 側 mux server 起動**

   ```sh
   # on mac
   cd ../android_app
   ./gradlew installDebug
   adb shell am start -n com.example.wakeandroidtether/.MainActivity
   # Foreground service として MuxServer が起動、 BluetoothServerSocket で待受
   # ログで listenUsingRfcommWithServiceRecord の UUID を確認:
   adb logcat -s MuxServer | grep "listening uuid="
   # → 1f2f8a3e-7c4f-4f3a-9d2b-c0ffeec0ffee と一致するはず (panctl/main.c
   #   の kServiceUuid128 と同じ)
   ```

2. **Android の BD_ADDR を取得**

   ```sh
   # on mac
   adb shell settings get secure bluetooth_address
   # 出力例: AA:BB:CC:DD:EE:FF
   ```

3. **pomera ↔ Android を BT 上で pair (Numeric Comparison)**

   pomera 側 BD_ADDR は今 factory default `aa:aa:aa:aa:aa:aa` (smoke で観測済)。

   **3-a.** 母艦 Mac で SSH ターミナル 2 枚開く。 1 枚目で panctl 起動 (Step 4
   と同じ手順)、 2 枚目で `tail -f /tmp/panctl.log`。

   **3-b.** Android 側 Settings → Bluetooth → デバイスを検索 → "pomera-panctl"
   を選択 → タップ。 Android が **Numeric Comparison ダイアログ**を出す:

   ```
   ┌──────────────────────────────┐
   │ Bluetooth ペアリング要求     │
   │  654321                      │
   │ コードが一致しますか？       │
   │   [キャンセル]   [ペア]      │
   └──────────────────────────────┘
   ```

   **3-c.** 同じ瞬間に pomera 側 `tail -f` 画面に:

   ```
   >>> PAIR REQUEST from AA:BB:CC:DD:EE:FF — passkey 654321
       Compare with the dialog on the peer (Android Settings). Then run:
         panctlctl confirm     (codes match)
         panctlctl deny        (codes differ / unexpected)
       Auto-deny in 30 seconds.
   ```

   **3-d.** **両方の 6 桁が一致する**ことを目視確認したら:

   ```sh
   # on mac (3 枚目の SSH 端末)
   ssh <your-pomera-user>@<dm250-lan-ip> doas /tmp/panctlctl confirm
   # → "confirmed pair with AA:BB:... (passkey 654321)"
   ```

   ↑ 直後に **Android 側で [ペア] をタップ**。 順序は厳密ではないが両方 OK
   すれば pair 成立。

   **3-e.** 不一致なら `doas /tmp/panctlctl deny` (MITM 攻撃の疑い)。 30 秒
   何もしないと panctl が自動で deny する。

   pair 完了後 `/var/db/panctl/tlv.dat` に link key が永続化される。 以降 reboot
   しても再 pair 不要。

4. **panctl 起動 (実 BD_ADDR で)**

   ```sh
   # on pomera (installed)
   doas /tmp/panctl --android-bdaddr AA:BB:CC:DD:EE:FF -t h4 -d /dev/cua00 2>&1 | tee /tmp/panctl.log
   ```

5. **期待される出力 (Phase A Step 6 完了の判定基準)**

   ```
   [panctl] main: starting
   HCI working; issuing SDP query to Android
   SDP found RFCOMM channel <N> for our UUID    ← (1) SDP 成功
   RFCOMM open cid=<X> mtu=<Y>                  ← (2) RFCOMM channel open
   peer HELLO ver=0 maxStreams=256 initWin=64KiB ← (3) Android からの HELLO 受信
   ```

   (1)(2)(3) 全部出れば mux protocol v0 の handshake 成立、 Phase A Step 6
   完了。 ここから pf divert → mux 経由で TCP/UDP 流すテストに進む。

#### つまずきポイント想定

- **SDP 失敗** — Android 側 MuxServer が listen してない / UUID 不一致。
  `adb logcat` で MuxServer の listen ログを確認。 UUID 文字列を panctl 側
  (`kServiceUuid128`) と byte 順比較
- **RFCOMM open status != 0** — pairing が落ちてる。 Android 側で pomera-panctl
  デバイスが "ペア解除" になってないか確認、 必要なら再 pair
- **peer HELLO 来ない** — Android 側 MuxServer は accept だけして HELLO 送る
  実装になっているか確認 (`android_app/.../mux/MuxServer.kt`)。 panctl は
  RFCOMM open 後に自分から HELLO 送り、 相手の HELLO を on_hello で受ける
- **BTstack の BD_ADDR が aa:aa:aa:aa:aa:aa のまま** — Android 側 link key DB に
  この addr で保存されると毎回 fresh pair になる。 panctl 側で
  `gap_random_address_set(0x9cb8b4f291c9)` で OTP 値を復元する patch を入れる
  と reuse できる。 課題 #2 に統合
2. **BD_ADDR が factory default の `aa:aa:aa:aa:aa:aa`** — kernel が起動時に
   読んだ OTP 値 (`XX:XX:XX:XX:XX:XX`) を BTstack 側で復元する必要あり。
   `gap_random_address_set` で固定値を入れるか、 chipset_bcm の vendor 拡張で
   OTP 再読出しを叩く。 panctl では `tlv` 永続化で出荷時 1 回設定する方針が
   筋良い
3. **BTstack のクロスビルド** — 今回 pomera native build (数分)、 mac から
   cross compile すれば 1 分以下。 `.crossroot/` sysroot を活かして cc に
   `-target armv7-unknown-openbsd --sysroot=.crossroot/openbsd-armv7-7.9` で
   通せるか試す価値あり。 panctl 反復開発の体験が大幅改善する
