# wake_android_tether

pomera DM250 (OpenBSD) からインターネットへ抜けるための「**Android テザリングを遠隔で起こす / 経路を確立する**」仕組みを調べる作業ディレクトリ。

親ワークスペース [`../README.md`](../README.md) は当初 **Bluetooth PAN** を想定していたが、Windows 11 + Samsung 端末の組み合わせには **「BT で Android 母艦の WiFi ホットスポットを ON にして、PC は WiFi で乗り入れる」** という別ルートが存在する（いわゆる Phone Link / Link to Windows の **Instant Hotspot**）。本ディレクトリではまずその仕組みを解析した。

> **現在の採用方針 (2026-05-26 第 2 改訂)**: 解析の結果、Instant Hotspot 路線も Shizuku + 自作アプリ路線もいずれも不採用となり、**「Android アプリで RFCOMM ↔ TCP/UDP mux protocol を自前実装、pomera 側は BTstack userland で RFCOMM client を持つ」** 構成 (PLANS.md の軸 **1-G + 3-E**) に収束した。pomera 側で Tailscale を動かす要件があり、SSH/tmux 用途には十分。詳細は [`PLANS.md`](PLANS.md) と [`DESIGN.md`](DESIGN.md)。
>
> 以下「Instant Hotspot の仕組み」と「pomera (OpenBSD) で同じことをするには」セクションは**当初検討時の解析資料**として残してある。実装方針の正は PLANS.md。

## ここに置いてある APK

| ファイル | パッケージ名 | 実体 |
|---|---|---|
| `Windows App_11.0.0.101_APKPure.apk` | `com.microsoft.rdc.androidx` | **Microsoft Remote Desktop クライアント** (旧 RD Client → 現 "Windows App")。Android から Cloud PC / Azure Virtual Desktop / Windows 365 等へ RDP 接続する**だけ**のクライアントで、テザリング制御機能は持たない |
| `Link to Windows_1.24082.214.0-canary_APKPure.apk` | `com.microsoft.appmanager` | **Phone Link / Link to Windows の Android 側コンパニオン**。BT 経由で WiFi ホットスポットを起こす機能はこちらに入っている |

> 名前は紛らわしいが「Windows App」と「Link to Windows」はまったく別物。テザリング自動起動の話に登場するのは後者。

## Instant Hotspot の仕組み (解析結果)

Link to Windows APK の `AndroidManifest.xml` から確定した事実だけを書く。

### 要求パーミッション (抜粋)

```
android.permission.BLUETOOTH
android.permission.BLUETOOTH_ADMIN
android.permission.BLUETOOTH_ADVERTISE
android.permission.BLUETOOTH_CONNECT
android.permission.BLUETOOTH_SCAN
android.permission.ACCESS_WIFI_STATE
android.permission.CHANGE_WIFI_STATE
android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE
com.microsoft.appmanager.permission.WRITE_SETTINGS
com.samsung.android.mdx.permission.GET_INSTANT_HOTSPOT_EVENT
deviceExperiences.permission.BLUETOOTH_TRANSPORT_EVENT
deviceintegration.permission.BLUETOOTH_TRANSPORT_EVENT
```

`Windows App` (RDP) 側には `BLUETOOTH_*` も `WRITE_SETTINGS` も `GET_INSTANT_HOTSPOT_EVENT` も一切宣言がない。テザリング起動に関与しないのはここで確定する。

### Samsung MDX 経由のフロー

Link to Windows の Manifest 中には Samsung 製の MDX (Multi-Device eXperience) SDK のクラスがそのまま組み込まれている:

- `com.samsung.android.sdk.mdx.bluetoothtransport.InstantHotspotSettingsProvider` (ContentProvider)
- `com.samsung.android.sdk.mdx.windowslink.bluetoothtransport.InstantHotspotSettingsProvider` (ContentProvider, Windows Link 派生版)
- `com.samsung.android.mdx.instanthotspot.ACTION_BT_SOCKET_CONNECTED` (Broadcast Intent)
- `com.samsung.android.mdx.instanthotspot.ACTION_BT_SOCKET_DISCONNECTED` (Broadcast Intent)
- `com.samsung.android.mdx.windowslink.Tile.onClick` (Quick Settings タイル)

Microsoft 側のグルーコードは `com.microsoft.mmx.agents.ypp.sidechannel.OemRfcommSideChannelBroadcastReceiver` などで、**YPP = Your Phone Platform** (Phone Link バックエンドの内部コードネーム) が Bluetooth RFCOMM をサイドチャネルとして使うために用意されている。

これを踏まえて推定される通信シーケンス:

```
[Windows 11 PC: Phone Link]
   │
   │ 1. ペア済み Samsung Galaxy を BT で発見
   │ 2. RFCOMM チャネルを開く (Samsung MDX が listen している UUID)
   │ 3. "hotspot を起こせ" を proprietary プロトコルで送出
   ▼
[Samsung MDX system component (OneUI 同梱、privileged)]
   │ 4. ACTION_BT_SOCKET_CONNECTED を GET_INSTANT_HOTSPOT_EVENT 持ちに broadcast
   │ 5. Link to Windows の InstantHotspotSettingsProvider に SSID/PSK 設定を問い合わせ
   │ 6. Samsung 専用 SoftAp API で WiFi ホットスポットを ON
   │ 7. 確定した SSID/PSK を RFCOMM ソケットに書き戻し
   ▼
[Windows 11 PC]
   │ 8. RFCOMM 経由で受け取った SSID/PSK で通常の WiFi 接続
   ▼
インターネット (キャリア回線)
```

### 重要な制約

1. **Samsung 端末でしか成立しない**
   - WiFi ホットスポットを実際に ON にする所だけは **Samsung MDX system component (privileged)** が担当している。これは Galaxy 機にプリインストールされている特権コンポーネントで、一般アプリの権限では肩代わりできない (`TETHER_PRIVILEGED` 不要)。
   - Manifest 中の OEM 別拡張 (`extcn`, `exthns`, `extop`, `hihonor` 等) には Instant Hotspot 相当の API が見当たらず、**非 Samsung 機ではこの機能は無効**。Pixel / Xiaomi / etc. では Link to Windows 経由でホットスポットを自動起動できない。

2. **PC 側プロトコルは undocumented**
   - PC ↔ Android の RFCOMM 上の payload は Microsoft 独自バイナリで、公開仕様はない。Phone Link (Windows 11) 以外のクライアントを書くには、RFCOMM のサービス UUID とメッセージフォーマットを実機トラフィックから逆解析する必要がある。

3. **何が "BT" で、何が "WiFi" か**
   - BT で運ばれるのは **制御チャネル** (ホットスポット ON/OFF コマンド + SSID/PSK)
   - 実際の IP 通信は **WiFi (ホットスポット → ステーション)** で流れる
   - つまり BT PAN とは別物。BT PAN は BT 上でそのまま IP を流すが、Instant Hotspot は BT を一瞬の制御パスとしてだけ使い、本線は WiFi。

## pomera (OpenBSD) で同じことをするには (当初検討)

| ルート | 当初の評価 | 2026-05-26 第 2 改訂後の扱い |
|---|---|---|
| **BT PAN** (親 README の当初プラン) | ◎ | △ 代替手段。`btpan(4)` は OpenBSD 6.0 で削除済みのため事実誤認。**1-H 退避案に降りた場合のみ復活** |
| **USB テザリング (RNDIS)** | ○ | ○ 緊急時保険として保持 (大容量転送 / 1-G/1-H 共倒れ時) |
| **Android WiFi ホットスポットを手動 ON → pomera から WiFi 接続** | ○ | △ 容量が要る場面の手動運用のみ |
| **Instant Hotspot を OpenBSD から逆引きで叩く** | △ (要研究) | ✕ 副作用大、Samsung 専用、優先度最下位 |
| **Android アプリ内で RFCOMM ↔ TCP/UDP mux (新)** | (当初未検討) | ◎ **採用 (軸 1-G)**。Tailscale on pomera を維持しつつ完全自動化。詳細 [`PLANS.md`](PLANS.md) |

## 結論 (当初時点 / 経緯保持のため残す)

- **Windows App** (RDP クライアント) はテザリング起動とは無関係。誤導されないよう README に明記。
- **Link to Windows** の Instant Hotspot は **Samsung 端末専用の特権機構** + **Windows 専用クライアント (Phone Link)** + **proprietary RFCOMM プロトコル** の三点セット。pomera (OpenBSD) でそのまま使うには無理がある。
- pomera 計画としては親 README の **BT PAN** を本命にする (← **当初時点。後述参照**)

## 現在の採用方針 (2026-05-26 第 2 改訂)

詳細経緯は [`PLANS.md`](PLANS.md) 参照。要点だけ:

1. **当初 (1-E + 2-A + 3-A)**: 自作アプリ + Shizuku で `TetheringManager.startTethering()` を叩いて WiFi ホットスポットを起動、pomera は WiFi で乗り入れ、pomera 側 BT は BlueZ 移植 → **第 1 改訂で失効**: 最新 Android が hidden API を shell uid からブロック
2. **第 1 改訂 (1-F + 2-B + 3-E)**: 手動 BT tethering トグル + Android 標準 BT PAN + BTstack userland → **第 2 改訂で繋ぎ案に降格**: 完全自動化要件 + Tailscale on pomera 固定要件で不十分
3. **第 2 改訂 (1-G + 3-E、現行)**: **Android アプリで RFCOMM ↔ TCP/UDP mux protocol**、pomera 側は **BTstack userland で RFCOMM client + mux client**。Tailscale は pomera で動かす (Android Tailscale 不安定のため)。退避案として 1-H (App 内 userland NAT) を保持

設計原則:
- ベンダー独自 / hidden API / signature 権限に乗らない (1-E 失効の教訓)
- Tailscale は pomera 側固定
- pomera 側の負荷最小化 (L4 forwarding のみ、IP stack なし)

## ファイル

- `Windows App_11.0.0.101_APKPure.apk` — 参考用 (RDP クライアント本体、後段 pomera→母艦 PC 接続で使う可能性はある)
- `Link to Windows_1.24082.214.0-canary_APKPure.apk` — Instant Hotspot 仕組み解析の対象。インストールはしていない
- [`android_app/`](android_app/) — **自作 Android アプリ (1-G mux server)**。RFCOMM 接続を受けて TCP/UDP を多重化、cell 回線へ socket(2) で抜く。signature 系特権ゼロ・Shizuku 非依存。2026-05-26 第 2 改訂で 1-E ベースから全面書き換え済み
- [`DESIGN.md`](DESIGN.md) — **pomera 側 BT スタックの設計と実装の指針**。BlueKitchen BTstack を userland で動かす設計、HCI 疎通手順、port notes、AP6212A pinmap、outbound intercept。関連: [`01-hci-bringup.md`](01-hci-bringup.md), [`02-btstack-port-notes.md`](02-btstack-port-notes.md), [`03-dm250-bt-pinmap.md`](03-dm250-bt-pinmap.md), [`04-outbound-intercept.md`](04-outbound-intercept.md), [`05-pairing-runbook.md`](05-pairing-runbook.md)
- [`PROTOCOL.md`](PROTOCOL.md) — **mux ワイヤ仕様 (v0 / draft)**。Android アプリと panctl で共有。frame format / TCP・UDP stream / flow control / ライフサイクル
- [`PLANS.md`](PLANS.md) — **経路設計の比較メモ**。第 2 改訂までの全変遷と未決事項。落ちた案も理由付きで残してある
