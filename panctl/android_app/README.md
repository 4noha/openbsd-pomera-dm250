# android_app — Android 側 BT mux server

PLANS.md 軸 **1-G** の Android 側実装。pomera (BTstack RFCOMM client) からの
RFCOMM 接続を受け、単一 BT チャネル上で **TCP/UDP を多重化** する mux サーバ。

- ワイヤ仕様: 親ディレクトリ [`../PROTOCOL.md`](../PROTOCOL.md)
- 設計の経緯: [`../PLANS.md`](../PLANS.md), [`../DESIGN.md`](../DESIGN.md)

> **2026-05-26 第 2 改訂で全面書き換え**。旧 1-E ベース (Shizuku + `TetheringManager`
> + 自前 ASCII プロトコル) は破棄。Shizuku / `TetheringManager` / `WifiManager`
> への依存はすべて削除し、**signature 系特権ゼロ**で動く構成にした。

## 動作要件

- **Android 12 (SDK 31) 以上**。Pixel / AOSP / Galaxy / Xiaomi いずれも可
- 必要 permission:
  - `BLUETOOTH_CONNECT` / `BLUETOOTH_ADVERTISE` / `BLUETOOTH_SCAN` (RFCOMM accept + SDP)
  - `INTERNET` (cell 側 socket outbound)
  - `FOREGROUND_SERVICE` / `FOREGROUND_SERVICE_CONNECTED_DEVICE` / `POST_NOTIFICATIONS`
  - `RECEIVE_BOOT_COMPLETED` (再起動時の自動復帰)
- **Shizuku / root / 特定 OEM 機能のいずれも不要**
- ペアリング: 初回のみ pomera と Settings → Bluetooth でペアリング (SSP / Secure Connections)

## ビルド

ホスト Mac で:

```bash
cd wake_android_tether/android_app

# 初回のみ Gradle ラッパー JAR を取得
gradle wrapper --gradle-version 8.7

./gradlew :app:assembleDebug
# → app/build/outputs/apk/debug/app-debug.apk
```

`gradle` が手元にない場合は `brew install gradle` か、Android Studio で
`wake_android_tether/android_app/` を Open すれば自動で wrapper が降ってくる。

インストール:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## 初期セットアップ (端末上)

1. このアプリを開き、上から順に:
   - `Request Bluetooth permissions`
   - `Request notification permission` (Android 13+)
2. 表示される **BT MAC** と **UUID** を控える。pomera 側 panctl の設定に必要
3. pomera 側からこの端末を **Settings → Bluetooth で先にペアリング**
4. `Start mux listener` を押して foreground service を起動

## プロトコル

PROTOCOL.md (親ディレクトリ) を参照。ワイヤフォーマットの実装は
[`mux/Frame.kt`](app/src/main/java/com/fournoha/wakeandroidtether/mux/Frame.kt) に集約。

## ディレクトリ構成

```
app/src/main/
├── AndroidManifest.xml
├── java/com/fournoha/wakeandroidtether/
│   ├── bt/
│   │   ├── Protocol.kt        ← SDP UUID / SDP name / proto version
│   │   └── RfcommService.kt   ← Foreground service + accept loop
│   ├── mux/
│   │   ├── Frame.kt           ← frame types, codec, endpoint encoding
│   │   ├── FrameWriter.kt     ← single writer, channel-serialized
│   │   ├── MuxServer.kt       ← read loop, frame dispatch
│   │   ├── TcpStream.kt       ← per-TCP-stream socket + window mgmt
│   │   └── UdpStream.kt       ← per-UDP-stream DatagramSocket + NAT TO
│   ├── prefs/Prefs.kt         ← listenerEnabled のみ保持
│   ├── boot/BootReceiver.kt   ← 再起動時に listener を復帰
│   └── ui/MainActivity.kt
└── res/...
```

## 認証モデル

v0 では BT pairing (SSP / Secure Connections) に全面依存。アプリ層では
peer mac の allow-list を持っていない (将来追加余地は PROTOCOL.md §6 に記載)。

## 既知の制約 / v0 トレードオフ

- **TCP backpressure が mux read を巻き込む可能性**: 単一 TCP stream の
  socket write が詰まると、mux 全体の受信が一時停止する設計。RFCOMM の
  実効帯域 (1〜2 Mbps) を考えると実害は限定的 (内部キューを足すなら v1 で)
- **IPv6 非対応**: PROTOCOL.md §3.4 で `addr_type=0x02` は予約のみ
- **DNS pass-through 非対応**: pomera 側で DoT 経由解決した IP のみ受ける
- **インバウンド listen 非対応**: Android 側で TCP/UDP listen はしない
- **ICMP 非対応**: 1-G の非目的。`ping` は通らない (Tailscale 用途では不要)

## 旧 1-E 機能の削除

以下は本書き換えで**完全に削除**された:

- `tether/TetherController.kt`
- `privileged/PrivilegedService.kt`
- `aidl/com/.../IPrivilegedService.aidl`
- Shizuku 依存 (`dev.rikka.shizuku:api` / `:provider`)
- `Prefs.token` (ASCII プロトコル時代の shared secret)
- ASCII line protocol (`PING/WAKE_ON/WAKE_OFF/OK/ERR`)
- `CHANGE_WIFI_STATE` / `ACCESS_WIFI_STATE` permission
