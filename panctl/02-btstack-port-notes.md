# 02 - BTstack を OpenBSD で動かす際の予想差分

[DESIGN.md](DESIGN.md) の Phase A Step 2 / Phase B Step 4 で BTstack 本体を OpenBSD で build するときに踏みそうな箇所の事前メモ。**手元で実際に build した結果ではなく、BlueKitchen BTstack の構造から推測した「ここを最初に見る」リスト**。実機で当たったら差分を確定させてここに追記する。

## 0. BTstack の port ディレクトリ構造 (要確認)

upstream `bluekitchen/btstack` の `port/` 配下に POSIX 系として以下があるはず:

- `port/libusb` — Linux/macOS で標準 HCI USB dongle と libusb で喋る
- `port/posix-h4` — POSIX TTY 経由で H4 transport (BCM/CSR/TI 等の UART chip)
- `port/posix-h5` — 同上だが 3-Wire H5 (eHCILL)。AP6212A は通常 H4 で OK
- `port/raspi` — Raspberry Pi 内蔵 BT (BCM43438) 向け。**AP6212A と最も近い**

**Phase A は `port/libusb`、Phase B は `port/posix-h4`** を出発点にする。 `port/raspi` は patchram + baud 切替を userland 側でやる前提だが、 **DM250 ではこれを kernel `bcmbt_fdt.c` が代行する**ので参考にする必要はない（[STATUS.md](STATUS.md) §「bring-up 分岐確定」）。 `port/posix-h4` をそのまま流用、 chipset 設定は **`chipset/none`**（`btstack_chipset_bcm_*` は呼ばない）。

着手時の最初の作業:

```sh
# on mac (planning) or openbsd (build)
git clone --depth 1 https://github.com/bluekitchen/btstack third_party/btstack
ls third_party/btstack/port/
cat third_party/btstack/port/libusb/Makefile.inc
cat third_party/btstack/port/raspi/Makefile.inc
```

## 1. 予想される OpenBSD 固有の引っかかり

### 1.1 ビルドシステム

- BTstack は **gmake** + 手書き Makefile (autoconf 系を使っていない)。OpenBSD では `gmake` を `pkg_add gmake` で入れる。
- ヘッダ・ライブラリパスが `/usr/local/include`, `/usr/local/lib`。Linux 既定の `/usr/include`, `/usr/lib` を直接見ている Makefile があれば `CFLAGS += -I/usr/local/include`, `LDFLAGS += -L/usr/local/lib` を追加する。
- `pthread` リンクは `-lpthread` で問題ないはず。

### 1.2 libusb 周り

- OpenBSD の libusb1 は **ugen(4) バックエンド**。Linux の usbfs と挙動が微妙に違う。
- 既知の差:
  - **hotplug** (`libusb_hotplug_register_callback`) が OpenBSD では NOTIMPLEMENTED な可能性。BTstack 側で hotplug を使っているとリンクは通っても実行時に効かない。最初は固定 device で繋ぐ前提にして hotplug を OFF。
  - **interrupt transfer のタイムアウト挙動**: OpenBSD は EBUSY を返すパターンがあるので BTstack の reschedule ロジック側で再試行が必要かも。
- libusb の async transfer (`libusb_submit_transfer`) は BTstack の HCI 読み取りループで多用される。OpenBSD で stuck しないかは Step 1 の smoke の延長で計測する。

### 1.3 termios (Phase B の UART transport)

- BTstack の `btstack_uart_block_posix.c` は `tcgetattr` / `tcsetattr` を使う。
- **DM250 では kernel が baud 切替しない**（115200 固定）ので、 userland 側は `cfsetspeed(&t, B115200)` だけ呼べばよい。 BCM 用 patchram 後の高速 baud (3 Mbps / 921600) を扱う旧懸念は失効（kernel が baud を上げないため）。
- RTS/CTS フロー制御: `CRTSCTS` は OpenBSD でも有効。AP6212A の UART は CTS/RTS 配線されており、 `bcmbt_fdt.c` も RTS pulse を打ったあと UART mode に戻すので、 ハード制御を有効にする。

### 1.4 シグナル / run loop

- BTstack POSIX port の run loop は **select(2) ベース**。OpenBSD でも普通に動く。kqueue 化は不要。
- ただし libusb の async 完了通知が file descriptor で取れるかどうかが run loop 統合の鍵。Linux の libusb は `libusb_get_pollfds()` を提供する。OpenBSD libusb1 も提供しているはずだが、**poll に挙げる fd セットが空の瞬間がある** ことに注意 (BTstack の port も `LIBUSB_TRANSFER_COMPLETED_CB_HANDLED` で対処済みのはず)。

### 1.5 タイマー

- `clock_gettime(CLOCK_MONOTONIC, …)` は OpenBSD で問題なく動く。BTstack のタイマー基盤はそのままで OK。

### 1.6 ファイルパス / 永続化

- BTstack の `btstack_tlv_posix.c` はリンクキーを単一ファイルに保存する。デフォルトパスが `~/.btstack/tlv.dat` 風なら問題なし。
- panctl では `/var/db/panctl/tlv.dat` 等の絶対パスに差し替えて、`rc.d` 起動時のオーナ問題を回避する。

## 2. BCM patchram のフロー (DM250 では kernel が実施)

**結論: DM250 では userland 側で patchram コードを書く必要はない**。 詳細は
[STATUS.md](STATUS.md) §「bring-up 分岐確定」。 boot 時に `bcmbt_fdt.c` が以下を完了させ、 userland に渡るのは「すでに HCI ready な 115200 8N1 UART (`/dev/cua00`)」だけ。

kernel `bcmbt_fdt.c` がやること (詳細はソース参照):

1. GPIO で shutdown-pin OFF/ON、 RTS pulse、 device-wakeup ON
2. UART を 115200 で初期化
3. `HCI Reset` 投入
4. `HCI Vendor Download Minidriver` (`0x2e 0xfc`) 投入
5. `loadfirmware(9)` で **`/etc/firmware/BCM4343A1.hcd`** を読み、 各エントリを HCI コマンドとして送信
6. post-firmware reset、 UART 再初期化（115200 のまま）
7. 仕上げの `HCI Reset` ＋ `Read BD_ADDR`

panctl 側に必要なのは:

- `port/posix-h4` の `btstack_uart_block_posix.c` をそのまま使う
- chipset は `chipset/none`（`hci_set_chipset(...)` 不要 or `btstack_chipset_none_instance()` を指定）
- BTstack の `btstack_chipset_bcm_*` API は **DM250 では呼ばない**
- termios: 115200 8N1 + CRTSCTS のみ設定。 baud 切替コードは書かない

amd64 + USB HCI ドングル (Phase A) の場合:

- BCM 系 USB ドングルだと chipset/bcm が **必要になることもある**（出荷時 firmware で動かないドングルがある）。 そのときだけ `btstack_chipset_bcm_instance()` を使う。 CSR8510 A10 は出荷時 firmware で動くので不要

AP6212A 用 patchram ファイル:

- 配布元の名前は `BCM43430A1.hcd` (チップ型番ベース、 RPi-Distro/bluez-firmware master)
- **OpenBSD カーネル `loadfirmware(9)` 期待名は `BCM4343A1.hcd`** (patchram 系列名ベース、 `bcmbt_fdt.c` で hardcode)
- バイナリ自体は同一。 リネーム配置が必要
- 配置先: `/etc/firmware/BCM4343A1.hcd`（kernel が読みに来る単一のパス。 panctl の引数で渡す必要はない）
- 詳細手順: [`etc/firmware.README.md`](etc/firmware.README.md)、 openBSD 側は `../../openBSD/install.md §6.2` に統合

## 3. RFCOMM client の組み立て

> PLANS.md 第 2 改訂で **1-G (App 内 mux)** を採用したことで、本セクションのスコープは PAN / BNEP から **RFCOMM client + 自前 mux protocol** に縮退した。PANU/BNEP の参考メモは末尾「退避案 1-H 用メモ」に残す。

BTstack の例で参考になるのは:

- **`example/spp_counter.c`** — SPP (Serial Port Profile = RFCOMM Channel) サーバ最小例。**ペアリング → RFCOMM チャネル受け入れ → byte stream read/write** のテンプレ
- **`example/spp_streamer_client.c`** (要 upstream 確認) — クライアント側。Android アプリの RFCOMM サーバに繋ぐので使うのはこちら。SDP browse → RFCOMM connect → データ送受信
- (1-H 移行時のみ) `example/panu_demo.c` — PANU + BNEP

RFCOMM クライアント実装の骨格 (擬似コード):

```c
static void packet_handler(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    switch (hci_event_packet_get_type(packet)) {
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            // SDP browse で Android アプリの RFCOMM channel 番号を探す
            sdp_client_query_rfcomm_channel_and_name_for_uuid(
                &handle_query_rfcomm_event, peer_addr, OUR_SERVICE_UUID);
        }
        break;
    case RFCOMM_EVENT_CHANNEL_OPENED:
        // mux client にチャネルハンドルを引き渡す
        mux_client_attach(rfcomm_event_channel_opened_get_rfcomm_cid(packet));
        break;
    case RFCOMM_DATA_PACKET:
        mux_client_on_recv(packet, size);
        break;
    }
}
```

`mux_client_*` は本ディレクトリ `panctl/mux_client.c` で実装し、RFCOMM チャネルハンドルを介してフレーミングと per-flow state (TCP stream_id テーブル / UDP NAT エントリ) を管理する。**BNEP / `tap(4)` / `tun(4)` / `dhclient` は一切持たない**。

SDP/UUID と mux ワイヤフォーマット: **既に [`PROTOCOL.md`](PROTOCOL.md) v0 / draft で凍結済み**。

- RFCOMM service UUID: `1f2f8a3e-7c4f-4f3a-9d2b-c0ffeec0ffee` (旧 android_app/bt/Protocol.kt 由来)
- SDP service name: `WakeAndroidTether`
- frame format: 6 バイトヘッダ (`ver:1 / type:1 / stream_id:2 / length:2`) + payload
- frame types: HELLO / BYE / PING / PONG / TCP_OPEN(_ACK) / TCP_DATA / TCP_CLOSE_WR / TCP_RST / TCP_WINDOW / UDP_BIND(_ACK) / UDP_PACKET / UDP_CLOSE
- credit-based flow control、graceful close、再接続セマンティクス含む

実装側は PROTOCOL.md §2-§4 をそのまま encode/decode する。テストベクタは PROTOCOL.md §9 にある。

## 4. ビルドターゲットのまとめ (Phase A)

```
panctl-libusb    ← Phase A 用: BTstack + libusb backend + RFCOMM + mux client
panctl-h4        ← Phase B 用: BTstack + posix-h4 + RFCOMM + mux client（patchram は kernel）
hci_smoke        ← 疎通 (01-hci-bringup.md)
bt_reg_on        ← (DM250 では不要、 kernel が GPIO bring-up を全部やる) — 残置するなら Phase A の他ハード検証用
```

`panctl` 本体は HCI transport を実行時切替できるよう、`-t libusb` / `-t h4 -d /dev/cua00` のような引数設計にする。 **`-f` (firmware path) は DM250 では使わない**（kernel が `/etc/firmware/BCM4343A1.hcd` を読む）。 Phase A の BCM 系 USB ドングルで patchram 必要な特殊ケースのみ `-f` が有効になる設計にしておく。

## 4.5 退避案 1-H 用メモ (BNEP / PAN へのフォールバック)

1-G の mux 実装で詰まり 1-H (App 内 userland NAT) に降りる時に必要になる:

- `example/panu_demo.c` をひな型に PANU 役で BNEP セッションを張る
- OpenBSD 側で `tap(4)` を開いて Ethernet モード (`TUNSIFMODE` に `TUN_LAYER2` か `IFF_LINK0`) で BNEP frame を 1:1 でブリッジ
- `dhclient tap0` で Android (NAP) から IP を取り、default route を設定
- 帯域・実装手間は 1-G より大きい (BNEP/L2CAP の状態管理 + IP stack 経由のオーバーヘッド)
- 採用判断は PLANS.md「現時点の採用方針」の退避条件参照

## 5. 確認したい upstream patch の有無

着手時に upstream を眺める段でやること:

- `git log --oneline --grep=BSD` / `--grep=OpenBSD` を BTstack で叩く。既存の OpenBSD 移植 PR がないか確認。
- `port/posix-*` の Makefile に `BSD` 分岐があるか。
- GitHub Issues で `openbsd` を検索。

もし誰かが既に下地を作っていれば、本ディレクトリの実装ボリュームは大幅に減る。
