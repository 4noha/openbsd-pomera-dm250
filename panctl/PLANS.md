# PLANS.md — 経路設計の比較メモ

pomera DM250 (OpenBSD) を Claude Code 専用シンクライアントにする計画の中で、「**どうやって Android テザリングを起こし、どうやって pomera までネットを引き込むか**」について検討した複数案を整理する。実装に着手していない案も「なぜ採らなかったか」を含めて残す (後で要件が変わった時に再評価しやすくするため)。

最終決定や状態は本書ではなく、各サブディレクトリの README / 親 [`../README.md`](../README.md) を正とする。

> **2026-05-26 第 1 更新**: 当初採用していた **1-E + 2-A + 3-A** は、**最新 Android が `TetheringManager.startTethering()` の hidden API を shell uid からブロック**したことで 1-E が実質不可となり再選定 → 一旦 **1-F + 2-B + 3-E** へ。
>
> **2026-05-26 第 2 更新**: 続いて「**完全自動化を維持したい**」「**Tailscale は pomera 側に置く** (Android 側 Tailscale が不安定との実測あり)」という二つの要件で 1-F (手動トグル) では不十分と判明。**Android アプリ内に RFCOMM ↔ TCP/UDP mux を実装する 1-G** を新規追加し本命に。退避案として **1-H (App 内に userland NAT、IP 透過)** を併記。1-F は繋ぎ案 / 緊急時保険として保留。新方針は本書末尾「現時点の採用方針」参照。各案のステータス行は後追いで足してあるので、過去の判断理由はそのまま残してある。

## 役割分担: 制御チャネルとデータ経路の分離

経路設計を考えるときは **2 つの独立した問題** に分解する。混ぜると詰む。

```
[pomera] ──(A) 制御チャネル──> [Android]
                                    │
                                    │ ホットスポット ON
                                    ▼
[pomera] <─(B) データ経路─── [Android] ──> インターネット
```

- **(A) 制御チャネル**: 「ホットスポット起こせ」「Stop」を運ぶ低帯域・低頻度・短命の通信。pomera の WiFi が貧弱でも届く必要がある。BT (RFCOMM か GATT) が筋
- **(B) データ経路**: 実 IP トラフィック。帯域が要る。WiFi が本命、BT PAN がフォールバック

採用案では **(A) RFCOMM 自作プロトコル + (B) WiFi (ホットスポット接続)** という構成。フォールバックとして **(B') BT PAN (BNEP)** も pomera 側で開ける運用にする想定。

> **2026-05-26 第 2 更新**: 1-G 採用後、この「制御 / データ分離」モデルは事実上解消する。1-G は **単一 RFCOMM チャネル上に制御と全データを混載**するため、軸 2 の「データ経路」は 1-G 内に内包される。分離スキームは旧プラン (1-E + 2-A) との対比と、将来の代替案検討枠組みとしてこの文書に残してある。

---

## 軸 1: 制御チャネル ─ どうやって BT 経由でテザリングを起こすか

### 1-A. Samsung MDX + Microsoft Phone Link を流用 ✕

- **どう動く**: Galaxy にプリインストールされた Samsung MDX system app (privileged) が、Phone Link (Windows) から BT RFCOMM で来た制御コマンドを受け、`TETHER_PRIVILEGED` 権限で Mobile Hotspot を ON にし、SSID/PSK を BT で返す
- **不採用理由**:
  - **Samsung 端末以外では動かない** (MDX system app が居ない)
  - PC 側プロトコルは Microsoft 独自 RFCOMM で、Phone Link 以外のクライアント (pomera) を作るには逆解析が必要
  - Phone Link は Windows 専用。Linux/OpenBSD クライアントは無い
- 詳細: [`README.md`](README.md) の「Instant Hotspot の仕組み」セクション

### 1-B. Google Instant Tethering + ChromeOS を流用 ✕

- **どう動く**: ChromeOS が Better Together で Android とアカウント単位ペアリング。BLE 上で CryptAuth v2 由来の暗号化チャネルを張り、Google Play Services (privileged) に「テザ ON」を命じる
- **メリット**: Pixel/AOSP 系でも動く (Samsung 以外でも OK)
- **不採用理由**:
  - 認可が **Google アカウント + デバイスアテステーション** に紐付いている
  - 鍵が **TPM/GSC バウンド** で、Chromebook を Developer Mode にしてもローカル profile から鍵を抜けない可能性が高い
  - pomera (OpenBSD) には BLE スタックが無い (BT スタック自体が無い)
  - Linux/BSD を Better Together のデバイスとして enroll する公式経路は無い

### 1-C. 自分の Chromebook から鍵を抜いてリバース ✕

- **どう動く**: 1-B のプロトコルを Chromium ソースから読み解き、自分の Chromebook の profile から CryptAuth 鍵を抽出して pomera (or Linux) クライアントに移植
- **不採用理由**:
  - Dev Mode 切替で stateful パーティションが wipe → 既存ペアリング情報が消える (再ペアリングは可能だがやり直し)
  - 新しめの Chromebook では鍵が GSC (Google Security Chip) で守られていて生バイト列が取れない見込み
  - 仮に成功しても OpenBSD 側に BLE スタックが必要で、結局 BT 移植問題に戻る
  - 工数が膨大 (Chromium の SecureChannel 実装移植)

### 1-D. Chromebook を中継器にする (鍵は触らない) △

- **どう動く**: 自分の Chromebook を Developer Mode + SSH で開け、Chrome の D-Bus / WebUI 自動化経由で Instant Tethering を**正規ルートで**発火させる。pomera は Chromebook と USB-C で直結し、Chromebook をルータ代わりにする
- **メリット**: 鍵問題を完全に回避できる。プロトコル逆解析不要
- **デメリット**:
  - Chromebook を常に随伴する前提 (荷物が増える)
  - Chromebook の Developer Mode 維持・Chromebook 自身のバッテリ問題
  - 「pomera だけでスリムに」のコンセプトから外れる
- **保留案**として残す。Android アプリ自作案が技術的に詰んだ時の Plan B

### 1-E. 自作 Android アプリ + Shizuku + RFCOMM ✕ (2026-05-26 失効)

- **どう動く**: 自前 RFCOMM サーバ (このリポの `android_app/`)。Shizuku の `shell` 権限で `TetheringManager.startTethering()` を直接呼ぶ。自前 ASCII プロトコルで `WAKE_ON <token>` → `OK SSID=... PSK=...` を返す
- **メリット**:
  - 端末を選ばない (Pixel / Galaxy / Xiaomi いずれも可)
  - プロトコルは自分で決められるので、相手側 (pomera / ESP32 / Linux) を作りやすい
  - Google / OEM の特権アプリ署名に依存しない
- **デメリット**:
  - Android 側に常駐アプリ + 起動毎に Shizuku の起動が必要 (非 root の場合)
  - `TetheringManager.startTethering()` は hidden API なので将来の Android 更新で壊れ得る (reflection は 1 ファイルに局所化済み)
- **当初の採用根拠**: 唯一「Samsung / Google の独占機能に乗っからない」道で、長期的に保守できると考えていた
- **2026-05-26 失効理由**: Pixel 9 Pro 上の最新 Android では Shizuku が起動しても **`TetheringManager.startTethering()` の経路自体がブロック**されることが判明。Google が hidden API を順次塞いでいる流れの一環で、機種依存ではなく構造的。別の Android 機に乗り換えても遠からず同じ壁が来る見込み

### 1-F. 制御チャネルを持たない (手動 BT tethering トグル) △ (2026-05-26 第 2 更新で繋ぎ案に降格)

- **どう動く**: Android 標準 UI の **Settings → Network → Hotspot & tethering → Bluetooth tethering** を出発前に 1 回手動 ON。pomera は BT PAN で繋ぐ。明示的なテザリング起動コマンドは持たない
- **重要 (確認済 2026-05-26)**: BT tethering (PAN NAP) の有効化はアプリからは**できない**。`BluetoothPan.setBluetoothTethering(true)` は framework に存在するが `@hide` + **`TETHER_PRIVILEGED` signature 権限**必須で、Wi-Fi テザリングと同じ壁。Shizuku でも届かない (shell uid には降りない)。**手指でのトグル ON が本質的に必要**で、これは Android が今後塞ぐ性質の API ではなく標準 UI なので長期的に安定
- **アプリで補助可能な範囲**: `Intent(Settings.ACTION_BLUETOOTH_SETTINGS)` 等で設定画面に deeplink、BT 接続イベント検知後のリマインダ通知、設定状態のリフレクション読み取り (将来塞がれ得る) ─ いずれも「ON にする」ことはできない
- **メリット**:
  - hidden API / Shizuku / root / 特定 OEM 機能のいずれにも依存しない
  - Android 標準 UI トグルなので **将来の OS 更新で塞がれにくい** (これが大きい)
  - pomera 側 BT 要件が「BNEP のみ」に縮退 (RFCOMM client / SDP browse / pairing UI 等の負担減)
- **デメリット**:
  - 外出前のひと手間 (BT トグル ON)
  - BT idle 中の Android バッテリ消費はある (数 mA、許容範囲)
  - WiFi ホットスポットの高帯域に乗りたい場面で自動手段がない (容認するか、別途手動で WiFi ホットスポット ON)
- **当初の採用根拠**: 1-E が最新 Android で塞がれた以上、**いかなる隠し API にも乗らない設計**にしないと同じ問題を繰り返す。手動 1 タップで済むなら自動化のメリットは小さい、と当初は判断
- **2026-05-26 第 2 更新で繋ぎ案に降格した理由**: 「完全自動化を維持したい」「Tailscale on pomera 固定 (Android 側 Tailscale 不安定)」という要件追加で 1-G (App 内 mux) を採用したため。1-F の「Android 標準 UI で塞がれにくい」利点は依然有効なので、**1-G の実装中の暫定運用 / 1-G で詰まった時の保険**として保留

### 1-G. Android アプリで RFCOMM ↔ TCP/UDP mux ◎ (2026-05-26 第 2 更新で採用)

- **どう動く**: Android アプリが BT RFCOMM サーバとして動き、**単一 RFCOMM 接続上に TCP open/close + UDP datagram を多重化する小さなフレーミングプロトコル**を乗せる。pomera 側は `pf divert-to` で全アウトバウンド TCP/UDP を userland mux client に流し、Android 経由でセル回線に出る。L4 forwarding (TCP/UDP) のみで L3/IP 処理は持たない
- **フレーミング (案)**: `[type:1][stream_id:2][len:2][payload]` 程度。type に `TCP_OPEN`/`TCP_DATA`/`TCP_CLOSE`/`UDP_PACKET` 等
- **詳細設計**: [`DESIGN.md`](DESIGN.md) に分離
- **メリット**:
  - **Tailscale on pomera を維持できる** (WireGuard UDP 直結路が生きる。DERP fallback に頼らない)
  - **完全自動**: pomera 起動 → BT 自動接続 → mux up → Tailscale up
  - **アプリ権限は signature 系ゼロ** (`BLUETOOTH_CONNECT` + `INTERNET` + `FOREGROUND_SERVICE` のみ。Android 14/15 でも将来でも壁が来ない)
  - Android 側に IP stack を持たないので 1-H (C) より大幅に軽い (L4 forwarding のみ、TCP state machine も自前再実装しない)
  - pomera 側 BT 要件が **RFCOMM client のみ** に縮退。PANU / BNEP / `tap(4)` ブリッジ全部不要
- **デメリット**:
  - 自前 mux プロトコル設計が必要 (~300 行レベル)
  - pomera 側に `pf divert-to` + userland mux client (~500 行) が必要
  - RFCOMM 帯域 1〜2 Mbps が天井 (SSH / tmux 用途には十分)
  - L3 透過ではないので ICMP echo (ping) や GRE 等は通らない (Tailscale には不要、運用上問題なしと判断)
- **採用根拠**: Tailscale on pomera 固定要件と「ベンダー独自パスにも隠し API にも乗らない」原則の両方を満たす唯一案。1-E のように Android API ロックダウンで死ぬリスクが無く、1-F のように手動操作も要らない。長期保守性が最も高い

### 1-H. Android アプリで userland NAT (退避案) △ (2026-05-26 第 2 更新で退避案として保持)

- **どう動く**: 1-G の上位互換。Android アプリ内に TCP/IP スタック (smoltcp の JVM/NDK 移植や lwIP 等) を持ち、pomera からは BNEP frame か生 IP datagram を受け取って Android の cell 側に NAT する
- **メリット**: 任意の IP プロトコルが透過 (ICMP / UDP / GRE / etc.)
- **デメリット**: 工数大 (TCP state machine の自前実装含む)、デバッグ困難
- **退避条件**: 1-G の実装中に「TCP/UDP 以外のプロトコルが必要」「mux protocol の framing が複雑化して 1-H と工数が逆転」「pomera 側で `pf divert-to` を介した L4 intercept がうまく動かない」のいずれかが判明した場合
- **`VpnService` は使えない**: VpnService は Android 自身の outbound を捕まえる仕組みで、外部デバイスからの inbound データを NAT する用途には噛み合わない。1-H は VpnService 抜きで自前 IP stack を持つ構成

---

## 軸 2: データ経路 ─ pomera に IP を引き込む

> **2026-05-26 第 2 更新**: 1-G 採用後、軸 2 は 1-G 内に内包されるため独立した選択肢として議論する意味は薄れた。下記は **1-G で詰まった場合 (1-H にも降りられない場合) の代替手段** として残してある。

### 2-A. WiFi ホットスポット △ (2026-05-26 降格 / 手動運用のみ)

- pomera が WiFi STA として Android のホットスポットに接続
- 帯域が出る (実効 30〜200 Mbps)
- 問題: pomera の WiFi が貧弱な場面でハマる (joshua stein 記事も Atheros の感度に苦言)

### 2-B. BT PAN / BNEP △ (2026-05-26 第 1 更新で昇格、第 2 更新で代替手段に降格)

- Android Settings → Hotspot & tethering → **Bluetooth tethering** を ON
- BNEP over L2CAP で IP を運ぶ。Android 標準機能で、こちら側のアプリ不要
- 帯域 1〜2 Mbps と低速だが、BT が繋がる場面では確実
- pomera 側は BlueZ 移植が BNEP/L2CAP まで含めば追加実装ほぼ不要
- 親 [`../README.md`](../README.md) の当初プラン (BT PAN) はこれ

### 2-C. USB テザリング (RNDIS / CDC-ECM) ○

- USB-C ケーブルで pomera と Android を直結
- 帯域 ◎、安定 ◎
- OpenBSD 側は `cdce(4)` で USB Ethernet ガジェットとして見える
- **デメリット**: ケーブル必須。pomera のシンプルさは保てるが、無線で済ませたい局面では使えない
- 緊急時 / 大容量転送時の保険として持つ

### 2-D. RFCOMM 上に IP を流す (自作トンネル) ✕

- 理屈の上では RFCOMM はバイトストリームなので PPP / TUN を被せれば IP は通る (旧 DUN プロファイル相当)
- **不採用理由**:
  - 帯域 1〜3 Mbps と BT PAN と変わらない or 遅い
  - Android 側を tethering subsystem の外で NAT ルータにする必要があり、root か大改造が必須
  - `VpnService` は outgoing 用で、incoming (BT 側) を NAT する向きに使えない
  - DUN プロファイルは Android 4.4 以降事実上死んでいる
- **割に合わない**。BT で運ぶなら BT PAN が正解

---

## 軸 3: pomera 側 BT スタックをどうするか

### 3-A. OpenBSD + Linux BlueZ 移植 △ (2026-05-26 降格 / 工数大)

- 現在 OpenBSD には BT スタックが無い (2016 の 6.0 で削除)
- 移植プロジェクトが進行中
- **必須要件**:
  - HCI ソケット層 (kernel)
  - L2CAP
  - RFCOMM (制御チャネル用) **または** GATT (Android 側に GATT 経路追加するなら)
  - BNEP (BT PAN フォールバック用)
  - `bluetoothd` 相当 (ペアリング)
  - DM250 の BT チップに対する HCI トランスポート + ファーム blob
- **実現したら**: Android アプリは現行のまま、pomera 側は 100 行未満の C か、シェル + `bluetoothctl` でクライアントが書ける
- リスク: 移植スコープが BLE のみだった場合、Android アプリに GATT 経路を追加実装 (作業量中)

### 3-B. ESP32 / nRF52 を BT 中継 MCU として使う ○

- pomera ──USB シリアル──> ESP32 ──BT──> Android
- BlueZ 移植が間に合わない / DM250 BT チップが対応されない時の代替
- ESP32 が $5、ESP-IDF で RFCOMM client を書ける
- **デメリット**: ハードが 1 個増える。携行性とのトレードオフ
- ただし USB を生やすだけなので pomera 体験は大きくは損なわれない

### 3-C. OS を Linux に切り替え △

- Alpine / Debian / NixOS を pomera に入れて BlueZ をフルに使う
- 親 README の OpenBSD 路線を放棄することになる
- 別軸の利点 (Linux のドライバ豊富さ) もあるが、ここでは扱わない

### 3-D. BT を諦めて USB テザリング ○

- 軸 2-C と同じ。BT スタック問題そのものを消す
- ケーブル容認できれば最強にシンプル

### 3-E. BlueKitchen BTstack を userland で動かす ◎ (2026-05-26 採用)

- **どう動く**: ユーザーランドプロセス `panctl` に [BTstack](https://github.com/bluekitchen/btstack) を static link し、HCI を libusb (開発時の amd64 + USB ドングル) または POSIX H4 + BCM patchram (DM250 実機の AP6212A) で喋り、BNEP frame を `tap(4)` にブリッジする
- **3-A に対する優位点**:
  - **OpenBSD カーネルを一切触らない**。AF_BLUETOOTH ソケットファミリ追加や BNEP/L2CAP/RFCOMM kernel モジュール移植が不要
  - AP6212A 用 `BCM43430A1.hcd` の patchram ロードが BTstack の `chipset/bcm/` に同梱済み — 3-A だと自前で書く分量
  - 工数感: 3-A が月単位なのに対し、libusb / termios / tap(4) のグルーで 2〜4 週間規模
- **デメリット**:
  - BTstack ライセンスは BSD-3 + 商用条項。pomera 個人 / OSS 用途は問題なし、商用配布時のみ要連絡
  - upstream に OpenBSD port が無いので glue は自前
- 詳細設計: [`DESIGN.md`](DESIGN.md)

---

## 現時点の採用方針 (2026-05-26 第 2 改訂版)

### 経緯

| 時点 | 採用方針 | 切り替え理由 |
|---|---|---|
| 2026-05-26 当初 | **1-E + 2-A + 3-A** | (新規策定) |
| 2026-05-26 第 1 改訂 | **1-F + 2-B + 3-E** | 1-E (Shizuku 経由 `TetheringManager.startTethering()`) が最新 Android で hidden API ブロックされ失効 |
| **2026-05-26 第 2 改訂 (現行)** | **1-G + 3-E** | 「完全自動化要件」「Tailscale on pomera 固定要件 (Android Tailscale 不安定)」で 1-F の手動トグルでは不十分。App 内 mux で UDP 直結を確保する 1-G を採用 |

### 現行方針

- **データ経路 + 制御**: 軸 **1-G** (Android アプリで RFCOMM ↔ TCP/UDP mux)
- **pomera BT スタック**: 軸 **3-E** (BTstack userland)
- **退避案**: 1-G で TCP/UDP 以外が必要 / pomera 側 pf divert に重大な障害 → 軸 **1-H** (App 内 userland NAT)
- **緊急時保険**: 1-G/1-H とも詰まった場合 → 軸 **1-F** (手動 BT tethering) + 軸 **2-B** (Android 標準 BT PAN) で接続可能性のみ確保
- **Tailscale 配置**: **pomera 固定** (Android 側 Tailscale が不安定との実測あり、これは要件として固定された制約)

### 旧方針との連続性

- 軸 3-E (BTstack userland) は第 1 改訂で採用済み。第 2 改訂で**スコープが PANU + BNEP + tap(4) ブリッジから RFCOMM client のみに縮小**したが採択自体は維持。詳細は [`DESIGN.md`](DESIGN.md) に反映
- 軸 1-F は 第 1 改訂の本命だったが、第 2 改訂で「**1-G 実装中の暫定運用 / 1-G で詰まった時の保険**」として降格。捨てていない
- 軸 1-E と 1-A〜1-D の不採用理由は変わらず (ベンダー独自パス / Google ロックイン / Chromebook 随伴 / hidden API ブロック)

### 設計原則の固定化

1. **ベンダー独自 / 隠し API / signature 権限に乗らない**。1-E が塞がれた教訓
2. **Tailscale は pomera 側に置く**。Android 側 Tailscale の不安定さを許容しない
3. **pomera 側の負荷最小化**。1-G が L4 forwarding のみ (1-H の IP stack なし) を選んだ理由

このメモの目的は、後日「やっぱり別案にしよう」となった時に**それぞれが落ちた理由をもう一度議論しないで済む**ようにすることなので、新しい情報や条件変更が出たら追記すること。

## 未決事項 / 監視すべき項目

### 1-G の実装に直接効く

- **mux プロトコル仕様策定**: フレーミング、stream_id 設計、UDP NAT timeout、flow control (バックプレッシャ)、再接続セマンティクス
- **pomera 側 `pf divert-to` 検証**: OpenBSD pf の divert-packet / divert-to で全アウトバウンド TCP/UDP を userland に持ち上げる構成が安定して動くか。`tun(4)` 併用も検討
- **DNS bootstrap**: Tailscale 初回起動の `controlplane.tailscale.com` 解決を **DoT (853/TCP) か DoH (443/TCP)** で済ます設定。`unbound` の `forward-tls-upstream: yes` 設定が一番楽
- **Android アプリ**: `android_app/` 配下を 1-E ベースから 1-G ベースに書き換え (mux server + per-flow TCP/UDP socket 管理)

### BTstack / DM250 ハード bring-up (1-G/3-E 共通)

- BTstack の OpenBSD ビルドで詰む箇所 (`port/libusb` の hotplug / termios 高速 baud)。予想差分は [`02-btstack-port-notes.md`](02-btstack-port-notes.md)
- DM250 の **AP6212A 内蔵 BT の UART インスタンス番号 / BT_REG_ON GPIO ピン**。`https://github.com/jcs/openbsd-src` (rk3128 ブランチ) DTB と `https://github.com/jcs/linux-dm250` dts が一次情報源
- `BCM43430A1.hcd` の入手元決定 (Raspberry Pi `bluez-firmware` / `linux-firmware/brcm` どちらでも可、SHA を記録)

### 監視事項

- Tailscale の WireGuard UDP path が 1-G の mux 上で実用速度を出すか (実測)。出ない場合は 1-G 内で DERP fallback に切替えるか、1-H に降りるかの判断
- Android アプリのフォアグラウンドサービスが OS killer に殺されないか (Doze 対策、`BLUETOOTH_CONNECT` の foreground service type 設定)
- 帯域: BT RFCOMM 1〜2 Mbps で詰まる用途が出たら 軸 2-A (手動 WiFi ホットスポット) か 2-C (USB テザリング) を併用
