# 01 - HCI 到達確認 (Phase A Step 1)

[DESIGN.md](DESIGN.md) の Phase A Step 1。**OpenBSD ホストから USB HCI ドングルに HCI コマンドを 1 発投げて応答が返るところまで** を確認する。

これが通れば BTstack 全体を載せる土台があると確定する。逆にここで詰まるなら、libusb / ugen(4) / dongle のどれかが死んでいるので先に解決する。

## 0. 前提

- 開発機: amd64 OpenBSD (snapshot 推奨。安定版でも可)。VM 上の場合は USB passthrough を有効化しておく。
- ホスト OS: 本ディレクトリは Mac 上で計画を書く場所。**手順自体は OpenBSD 実機 (or VM) で叩く**。コードブロック頭に `# on openbsd` と明記する。
- 必要なもの:
  - **標準 HCI USB ドングル**: USB Class `0xE0` / Subclass `0x01` / Protocol `0x01` (Wireless Controller / Radio Frequency / Bluetooth)。CSR8510 A10 や Realtek RTL8761B 系で OK。
  - **NG なもの**: Creative BT-W3 や類似の "BT audio transmitter" 製品 (USB Audio Class として見える → HCI 不可)。

## 1. ドングル装着確認

```sh
# on openbsd
$ usbdevs -v
```

期待される行 (CSR8510 の例):

```
addr 02: 0a12:0001 Cambridge Silicon Radio, CSR8510 A10
        super speed, self powered, config 1, rev 0.10, iSerial -
        driver: ugen0  ← これが出れば libusb から触れる
```

ポイント:

- `driver: ugen0` が attach されること。`driver: ubt0` ではない (OpenBSD は `ubt(4)` を持たないのでそもそも attach しようがない)。
- 出ない場合: `dmesg | tail` で USB enumeration エラーが無いか、`usbhidaction(1)` 等が掴んでいないか確認。

権限: `/dev/ugen0.*` はデフォルト root 所有。後段で libusb から触るので、開発中は `chgrp operator /dev/ugen0.*` + `chmod g+rw` で十分。本番 panctl は専用ユーザを作る (DESIGN.md Phase A Step 7 で詰める)。

## 2. libusb のインストール

```sh
# on openbsd
$ doas pkg_add libusb1
```

ヘッダは `/usr/local/include/libusb-1.0/libusb.h`、ライブラリは `/usr/local/lib/libusb-1.0.so`。OpenBSD の libusb1 は `ugen(4)` バックエンドで動く。

## 3. 疎通スクリプト (HCI_Reset 1 発)

`tools/hci_smoke.c` として置く予定の最小コード。HCI コマンドのうち最も無害な `HCI_Reset (OGF=0x03, OCF=0x003)` を投げて、`Command Complete` イベントが返れば成功。

要点 (擬似コード):

```c
// OpenBSD で gcc -I/usr/local/include -L/usr/local/lib -lusb-1.0
libusb_init(NULL);
libusb_device_handle *h = libusb_open_device_with_vid_pid(NULL, 0x0a12, 0x0001);
libusb_claim_interface(h, 0);

// HCI Command on Control endpoint (bmRequestType=0x20, bRequest=0)
uint8_t cmd[3] = { 0x03, 0x0c, 0x00 };  // opcode 0x0c03 = HCI_Reset, plen=0
libusb_control_transfer(h, 0x20, 0, 0, 0, cmd, sizeof cmd, 1000);

// Event on Interrupt IN endpoint 0x81
uint8_t evt[260]; int len;
libusb_interrupt_transfer(h, 0x81, evt, sizeof evt, &len, 1000);
// 期待: evt[0]=0x0e (Command Complete), evt[3..4]={0x03,0x0c}, evt[5]=0x00 (success)
```

これが返らない時の典型原因:

| 症状 | 原因 | 対処 |
|---|---|---|
| `LIBUSB_ERROR_ACCESS` | `/dev/ugen0.*` のパーミッション | `chmod g+rw` か doas で実行 |
| `LIBUSB_ERROR_BUSY` | 他プロセスが claim 中 | `fstat /dev/ugen0.*` で確認 |
| Control transfer は成功するが Event が来ない | エンドポイント番号が違う dongle | `lsusb -v` 相当を `usbdevs -vd ugen0` で取って Interrupt IN の bEndpointAddress を確認 |
| Event が来るが status != 0x00 | ドングル側 firmware の事情 | 別ドングルで再試行 |

## 4. 通ったらここで止める

この時点で **「OpenBSD 上で BT HCI を喋れる状態」が確定** する。次のステップ ([02-btstack-port-notes.md](02-btstack-port-notes.md) の build 確認 → BTstack 例の `inquiry` 実行) に進む。

ここで詰まったら DESIGN.md の Phase A は塩漬けになるので、無理に先に進まず原因を突き止める。

## 5. メモ

- macOS や Linux で同じ smoke を先に通しておくと、ドングル個体不良の切り分けが楽。
- VM (qemu + USB passthrough) で進める場合、host 側の bluez が dongle を握ってしまうことが多い。`sudo rmmod btusb` 等で剥がしてから VM に渡す。
- CSR8510 系は中華クローンに「CSR を名乗るが実は別チップ」が混ざる。HCI_Read_Local_Version_Information (`0x1001`) で manufacturer name を一度見ておく。
