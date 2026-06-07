# PROTOCOL.md — RFCOMM Mux Protocol (両端共有仕様)

PLANS.md 軸 **1-G** の実装で、単一 RFCOMM チャネル上に **TCP コネクション + UDP データグラム** を多重化するためのフレーミング仕様。pomera 側 (`panctl/`) と Android 側 (`android_app/`) でこの仕様を共有する。

> **状態**: **v0 / draft**。実装着手前の合意ドキュメント。Phase A Step 5 (BTstack + libusb で疎通) に進む段階で v1 に昇格して凍結する。それまでは破壊的変更を許す。

## アーキテクチャ概観

本仕様は **pomera 側で出す TCP/UDP outbound を Android 側で代理 socket(2) する**ための多重化プロトコル。役割は固定で非対称:

| 端末 | 役割 | 責務 |
|---|---|---|
| **pomera (panctl)** | **mux client** | `pf divert-to` で取り上げた outbound flow ごとに `TCP_OPEN` / `UDP_BIND` を発行、stream_id を割り当てる |
| **Android (RfcommService)** | **mux server** | 受け取った宛先で実 `Socket` / `DatagramSocket` を open、両方向のバイト転送を仲介 |

データ経路 (1 本の RFCOMM チャネル上に折り畳まれる):

```
[pomera 上のアプリ]                                  [リモート (Tailscale 等)]
        │                                                    ▲
        │ socket(2) outbound                                 │
        ▼                                                    │
[OpenBSD カーネル: TCP/UDP stack + pf divert-to]            │
        │                                                    │
        ▼  (TCP segment / UDP datagram)                      │
[panctl: mux client]                                         │
        │                                                    │
        │ TCP_OPEN / TCP_DATA / UDP_BIND / UDP_PACKET (フレーム)│
        ▼                                                    │
[BTstack: RFCOMM single channel]  ◀──── BT link ────▶  [Android BluetoothSocket]
                                                             │
                                                             ▼
                                                  [RfcommService → MuxServer]
                                                             │
                                                             │ frame decode + per-stream dispatch
                                                             ▼
                                                  [Android JVM: Socket / DatagramSocket]
                                                             │
                                                             ▼
                                                  [Android カーネル: cell 経由 outbound]
                                                             │
                                                             └──> インターネット
```

### 設計上の不変条件

- **IP stack は両端どちらも持たない**。pomera は OpenBSD カーネルの socket API、Android は JVM の `Socket`/`DatagramSocket` を使う。本プロトコルは **L4 (TCP/UDP) より上の転送だけ**を扱う。ICMP や生 IP は流さない (1-G の非目的)
- **stream の発行は pomera 側のみ** (v0)。Android 側は `TCP_OPEN`/`UDP_BIND` を**送らない**。インバウンド listen の用途が出たら v1 で別 frame type を追加
- **stream_id は pomera が割り当てる**。再接続で番号空間がリセットされる (§4.2)
- **複数 stream は単一 RFCOMM チャネル上で時分割多重**。frame ごとに `stream_id` で振り分ける。stream 間の公平性は frame サイズ上限 (4 KiB 推奨, §2.1) と per-stream credit window (§3.9) で確保
- **mux protocol 自体は無認証・無暗号**。BT link layer の SSP / Secure Connections と、上位の Tailscale / SSH が end-to-end で守る (§6)

### コンポーネントと参照ドキュメント

| コンポーネント | 場所 | 参照 |
|---|---|---|
| pomera 側 mux client + RFCOMM | `panctl/` (本ディレクトリ) | [`DESIGN.md`](DESIGN.md) |
| outbound intercept (`pf divert-to`) | OpenBSD 設定 + panctl glue | [`04-outbound-intercept.md`](04-outbound-intercept.md) |
| Android 側 mux server | `android_app/` 配下 | [`android_app/README.md`](android_app/README.md) |
| frame コーデック (Kotlin) | `android_app/.../mux/Frame.kt` | 本仕様の §2 / §3 を実装 |
| DNS bootstrap | unbound + DoT | [`etc/unbound.conf.sample`](etc/unbound.conf.sample) |

## 0. 目的と非目的

### 目的

- pomera 上の **アウトバウンド TCP / UDP** を 1 本の RFCOMM stream 上で Android に転送し、Android がキャリア回線へ socket(2) で出る
- Tailscale の WireGuard UDP path (NAT 越え経路含む) を通せる帯域・遅延特性を提供する
- 接続多重化 (複数の TCP / UDP セッション同時)、フロー制御、graceful close、再接続耐性

### 非目的

- L3 透過 (ICMP echo, GRE, IP-in-IP は通さない)。必要になったら退避案 1-H へ
- 信頼性: RFCOMM 自体が L2CAP 上で reliable・順序保証付き。本プロトコルは frame loss を仮定しない
- 暗号化: pomera ↔ Android 間は **BT link-layer 暗号** (SSP / Secure Connections) に委ねる。本プロトコル層では追加暗号を持たない (上位の Tailscale / SSH が end-to-end で暗号化する前提)
- インバウンド (Android → pomera) の TCP listen 提供: pomera は client roleのみ。将来必要になったら別 stream type で追加

## 1. トランスポート前提

- 下位レイヤ: **Bluetooth RFCOMM (single channel)** over L2CAP
- 信頼性: 順序保証あり、ロスなし。frame 境界は **本プロトコル側で明示的に持つ** (RFCOMM はバイトストリームなので、L2CAP DLCI 単位の境界はあてにしない)
- 想定 MTU: RFCOMM 上の **最大 payload は 1003 bytes 程度**だが、本プロトコルでは frame サイズに依存しない設計にする (ヘッダで length を持つ)
- 帯域: 実効 1〜2 Mbps を想定。アプリ側のバッファや credit window はこれに合わせる
- ペアリング: BT SSP / Secure Connections (Android 12+ 要件)。本プロトコル層はペア済み前提

## 2. フレーム形式 (binary, network byte order = big-endian)

すべてのフレームは固定 6 バイトのヘッダ + 可変長 payload。

```
 0           1           2           3           4           5
+-----------+-----------+-----------+-----------+-----------+-----------+
|   ver=1   |   type    |       stream_id       |       length          |
+-----------+-----------+-----------+-----------+-----------+-----------+
|                          payload (length bytes)                       |
|                                ...                                    |
+-----------------------------------------------------------------------+
```

| フィールド | サイズ | 内容 |
|---|---|---|
| `ver` | 1 byte | プロトコルバージョン。**v0 / draft 期間中は `0x00`**、v1 凍結時に `0x01` に昇格 |
| `type` | 1 byte | フレームタイプ (§3) |
| `stream_id` | 2 bytes | 16-bit 符号なし。制御フレーム (HELLO/PING/PONG/BYE) は `0x0000`。データフレームは 0x0001 〜 0xFFFF |
| `length` | 2 bytes | payload バイト数 (0 〜 65535)。0 でも legal |

**ヘッダの不正値** (例: 未知の `ver`、`length` が極端に大きい等) を受け取った側は **即座に BYE を送り RFCOMM をクローズ**する。再接続時に v0 → v1 のネゴが要れば HELLO で交換 (§3.1)。

### 2.1 stream_id の管理

- **クライアント (pomera) が単調増加で割り当てる**。`0x0001` から開始、ラップした場合は再使用 (Android 側が close 済みであることを期待)
- Android 側は自発的に stream_id を発番しない (v0 ではインバウンド非対応)
- TCP と UDP の stream_id 空間は **共有** する。stream_id だけでは TCP / UDP の区別ができないので、frame `type` で必ず判別する

### 2.2 frame chunking

- 単一論理データ (例: 大きな TCP write) は **複数の DATA フレームに分割**できる。受信側は順序保証付きで結合する
- 単一 frame の `length` 上限は **4096 bytes 推奨** (RFCOMM MTU と相性が良く、他 stream を長時間 starve しない)。実装上の hard limit は 65535
- 同一 stream の frame 順序は RFCOMM が保証するので、シーケンス番号は持たない

## 3. フレームタイプ

`type` byte の割当 (16進):

| 値 | 名前 | 方向 | payload 概要 |
|---|---|---|---|
| `0x01` | `HELLO` | 双方向 | 接続立ち上げ・バージョンネゴ (§3.1) |
| `0x02` | `BYE` | 双方向 | 接続終了通告 (§3.2) |
| `0x03` | `PING` | 双方向 | keepalive (§3.3) |
| `0x04` | `PONG` | 双方向 | keepalive 応答 (§3.3) |
| `0x10` | `TCP_OPEN` | C→S | TCP 接続要求 (§3.4) |
| `0x11` | `TCP_OPEN_ACK` | S→C | TCP 接続結果 (§3.5) |
| `0x12` | `TCP_DATA` | 双方向 | TCP データ転送 (§3.6) |
| `0x13` | `TCP_CLOSE_WR` | 双方向 | 半閉じ通告 (§3.7) |
| `0x14` | `TCP_RST` | 双方向 | 異常終了 (§3.8) |
| `0x15` | `TCP_WINDOW` | 双方向 | credit 更新 (§3.9) |
| `0x20` | `UDP_BIND` | C→S | UDP 仮想 socket 確保 (§3.10) |
| `0x21` | `UDP_BIND_ACK` | S→C | UDP_BIND 応答 (§3.11) |
| `0x22` | `UDP_PACKET` | 双方向 | UDP データグラム (§3.12) |
| `0x23` | `UDP_CLOSE` | 双方向 | UDP 仮想 socket 解放 (§3.13) |

未定義の `type` を受け取ったら BYE で切断。v0 期間中は予約値の意味付けを行わない。

「方向」凡例: **C** = pomera (client / mux client)、**S** = Android (server / mux server)。

### 3.1 HELLO

- `stream_id = 0`
- payload (6 bytes):
  ```
  +-----------+-----------+-----------+-----------+-----------+-----------+
  | proto_ver | flags     |   max_streams (u16)   |  initial_win (u16)    |
  +-----------+-----------+-----------+-----------+-----------+-----------+
  ```
  | 名前 | サイズ | 内容 |
  |---|---|---|
  | `proto_ver` | 1 byte | 送信側がサポートする最高 version (現状 `0x00`) |
  | `flags` | 1 byte | 全 bit 予約 (`0x00`) |
  | `max_streams` | 2 bytes | 自端末側で同時に受けられる stream 数 (デフォルト 256) |
  | `initial_win` | 2 bytes | 自分の受信ウィンドウ初期値 KiB 単位 (デフォルト 64 = 64 KiB) |
- フロー: 両端が接続後即座に HELLO を送る (server も client も)。version は `min(両端の proto_ver)` で確定
- HELLO 不一致 (proto_ver=0xFF 等) や `initial_win=0` は BYE 案件

### 3.2 BYE

- `stream_id = 0`
- payload (2 bytes): `reason_code (u16)`。`0x0000` = graceful、それ以外はエラー
- 送信側は payload 送出後ただちに RFCOMM を close してよい

### 3.3 PING / PONG

- `stream_id = 0`
- payload (8 bytes): nonce (任意、PONG では同じ nonce をエコー)
- どちらの端も独立に PING を送ってよい。30 秒間隔推奨
- 連続 3 回 PONG が返らなければ BYE → reconnect

### 3.4 TCP_OPEN (C→S)

- `stream_id = X` (新規割当)
- payload:
  ```
  +-----------+-----------------------+-----------+----...----+
  | addr_type |       dst_port (u16)  | addr_len  | addr bytes|
  +-----------+-----------------------+-----------+----...----+
  ```
  | 名前 | サイズ | 内容 |
  |---|---|---|
  | `addr_type` | 1 byte | `0x01` = IPv4、`0x02` = IPv6 (将来)、`0x03` = domain (将来) |
  | `dst_port` | 2 bytes | TCP 宛先ポート |
  | `addr_len` | 1 byte | 続く `addr bytes` の長さ。v4=4、v6=16 |
  | `addr bytes` | `addr_len` bytes | IPv4 4 bytes / IPv6 16 bytes |
- pomera 側で **DNS は事前解決済み** が前提 (DoT/DoH 経由)。`addr_type=0x03` (domain) は将来拡張用、v0 では送らない
- `dst_port = 0` は不正

### 3.5 TCP_OPEN_ACK (S→C)

- `stream_id` = 対応する OPEN と同じ
- payload (2 bytes): `status (u16)`
  - `0x0000` = OK (以後 TCP_DATA が流れる)
  - `0x0001` = ECONNREFUSED
  - `0x0002` = EHOSTUNREACH / ENETUNREACH
  - `0x0003` = ETIMEDOUT
  - `0x0004` = EAFNOSUPPORT (v0 で v6 を送ったとき等)
  - `0x00FF` = その他 (詳細はログのみ)
- status != 0 の場合、stream_id は **即座に閉じる**。RST 不要

### 3.6 TCP_DATA

- `stream_id` = 開設済み TCP stream
- payload: 任意のバイト列
- フロー制御: §3.9 参照。受信ウィンドウを超える送信は **送信側のバグ**。受信側は超過時に RST で切る
- `length = 0` の DATA は禁止 (close は CLOSE_WR / RST で行う)

### 3.7 TCP_CLOSE_WR

- 送信者が「自分の側は **これ以上書かない**」と通告する半閉じ
- payload: なし (`length = 0`)
- 両端から CLOSE_WR が出揃った時点で stream を解放
- TCP の FIN に相当。socket(2) 上では `shutdown(fd, SHUT_WR)` でマップする

### 3.8 TCP_RST

- 異常終了。stream を即座に破棄
- payload (2 bytes): `reason_code (u16)`
  - `0x0001` = peer reset
  - `0x0002` = idle timeout (Android 側で N 秒データなし等)
  - `0x0003` = flow control violation
  - `0x00FF` = その他
- RST を送ったらその stream_id への以後のフレームは無視

### 3.9 TCP_WINDOW

- credit-based flow control の credit 増量通告
- payload (4 bytes): `credit (u32)`、bytes 単位
- セマンティクス:
  - HELLO の `initial_win` で **自端末の受信ウィンドウ** を相手に伝える (KiB 単位)
  - 相手はそのウィンドウを超えて TCP_DATA を送ってはならない
  - 受信側がデータを下位 socket に書き出して空きが出たら、TCP_WINDOW で「あと N バイト送ってよい」を返す
- 初期ウィンドウのまま固定運用も可 (HELLO の initial_win が十分大きい場合)

### 3.10 UDP_BIND (C→S)

- `stream_id = X` (新規割当)
- payload: なし (`length = 0`)
- セマンティクス: 「これから stream_id=X 経由で **複数の UDP パケットを送る**。Android 側は ephemeral UDP socket を 1 個確保し、stream_id と紐づけてくれ」
- Android 側の socket は `bind(INADDR_ANY, 0)` する。NAT timeout は **120 秒**でアイドル切断

### 3.11 UDP_BIND_ACK (S→C)

- payload (2 bytes): `status (u16)` (TCP_OPEN_ACK と同一の値域)
- `0x0000` 以外なら stream_id は閉じる

### 3.12 UDP_PACKET

- `stream_id` = BIND 済み UDP stream
- payload:
  ```
  +-----------+-----------------------+-----------+----...----+----...----+
  | addr_type |     remote_port (u16) | addr_len  | addr bytes| datagram  |
  +-----------+-----------------------+-----------+----...----+----...----+
  ```
  - `addr_type` / `remote_port` / `addr_len` / `addr bytes` は TCP_OPEN と同形式
  - C→S 方向: `remote_*` は **送信先**
  - S→C 方向: `remote_*` は **受信元** (Android 側 socket に届いた datagram の `recvfrom` 結果)
- `datagram` 部分の長さ = `length - (1 + 2 + 1 + addr_len)`
- 単一 UDP_PACKET = 単一 datagram。フラグメント不可 (UDP セマンティクス維持)

### 3.13 UDP_CLOSE

- pomera が UDP stream を閉じる、または Android が NAT timeout で閉じた通告
- payload: なし
- close 後、対応 stream_id への UDP_PACKET は無視

## 4. ライフサイクル

### 4.1 接続立ち上げ

```
[pomera]                                   [Android]
   |                                          |
   |   RFCOMM connect (SDP UUID 1f2f8a3e...)  |
   |─────────────────────────────────────────>|
   |   RFCOMM channel opened                  |
   |<─────────────────────────────────────────|
   |                                          |
   |   HELLO (proto_ver=0, ...)               |
   |─────────────────────────────────────────>|
   |   HELLO (proto_ver=0, ...)               |
   |<─────────────────────────────────────────|
   |                                          |
   |   (mux ready, pf divert open)            |
```

両 HELLO 完了までは TCP_OPEN/UDP_BIND を送らない。

### 4.2 RFCOMM 切断 / 再接続

- RFCOMM が切れたら **すべての stream は即死**。recovery しない
- pomera 側 panctl は exponential backoff (1, 2, 4, 8, 30 秒 cap) で再接続を試行
- 再接続後は **新しい stream_id 空間** (`0x0001` から再開)。古い stream_id は使い回さない
- Tailscale / SSH 上位レイヤから見るとネットワーク瞬断。WireGuard の再ハンドシェイク (~1 秒) で復旧

### 4.3 graceful shutdown

- 任意の側が BYE を送って RFCOMM close
- BYE 前にアクティブな stream は **すべて破棄**。CLOSE_WR/RST の整理は不要 (相手側もまとめて discard する)

## 5. SDP / UUID

- RFCOMM service UUID: **`1f2f8a3e-7c4f-4f3a-9d2b-c0ffeec0ffee`** (旧 `android_app/bt/Protocol.kt` から流用)
- SDP service name: `WakeAndroidTether`
- RFCOMM channel 番号: SDP browse で動的解決 (固定しない)
- v0 / draft 期間中は UUID 変更可。v1 凍結時に確定

## 6. セキュリティモデル

- BT pairing で SSP / Secure Connections (Numeric Comparison) を経た上での通信前提
- mac アドレスベースの allow-list を Android アプリ側で持つ (将来):
  - 最初の HELLO 受信時点で `BluetoothSocket.getRemoteDevice().getAddress()` を見る
  - 設定で許可した MAC のみ HELLO に応答、それ以外は即 BYE
- v0 では allow-list 非実装。pairing 済み端末は全部許可

## 7. 実装メモ

### 7.1 pomera 側 (BTstack + mux client)

- BTstack の `spp_streamer_client.c` をベースに、`RFCOMM_DATA_PACKET` ハンドラ内で frame parser を呼ぶ
- frame parser は state machine: `READ_HEADER` (6 bytes) → `READ_PAYLOAD` (length bytes) → dispatch
- per-stream state: `HashMap<u16, StreamState>` (kqueue 不要、stream 数 256 想定)
- TCP socket は `pf divert-to` から取り出す。詳細は [`04-outbound-intercept.md`](04-outbound-intercept.md)

### 7.2 Android 側 (Kotlin + foreground service)

- `BluetoothSocket.getInputStream()` から read。`DataInputStream` で big-endian decode
- per-stream socket は `Socket`(TCP) / `DatagramSocket`(UDP) を coroutine + Dispatchers.IO で運用
- frame 送信は単一の write coroutine (mutex) で順序を守る

## 8. 拡張余地 (v1 以降に検討)

- IPv6 (`addr_type=0x02`)
- DNS pass-through (`addr_type=0x03` で domain を投げて Android 側で解決) — 性能比較してから採否判断
- インバウンド listen (Android → pomera) — 用途出てきたら
- Per-stream priority (制御 stream を優先)
- フレーム圧縮 (deflate)。SSH/Tailscale は既に圧縮済みなので不要寄り

## 9. テストベクタ (実装側で再現)

実装が両端で一致するか確認するための golden bytes (16進)。

### 9.1 HELLO (proto_ver=0, max_streams=256, initial_win=64)

```
00 01 00 00 00 06 00 00 01 00 00 40
└┬┘└┬┘└─┬─┘└─┬─┘└─┬─┘└┬┘└─┬─┘└─┬─┘
 │  │   │    │    │   │   │    └ initial_win=0x0040 = 64
 │  │   │    │    │   │   └ max_streams=0x0100 = 256
 │  │   │    │    │   └ flags=0x00
 │  │   │    │    └ proto_ver=0x00
 │  │   │    └ length=6
 │  │   └ stream_id=0
 │  └ type=0x01 (HELLO)
 └ ver=0
```

### 9.2 TCP_OPEN (stream_id=1, dst=8.8.8.8:443)

```
00 10 00 01 00 08 01 01 BB 04 08 08 08 08
└┬┘└┬┘└─┬─┘└─┬─┘└┬┘└─┬─┘└┬┘└─────┬─────┘
 │  │   │    │   │   │   │       └ addr bytes (4 bytes) = 8.8.8.8
 │  │   │    │   │   │   └ addr_len = 4
 │  │   │    │   │   └ dst_port = 0x01BB = 443
 │  │   │    │   └ addr_type = 0x01 (IPv4)
 │  │   │    └ length = 8
 │  │   └ stream_id = 1
 │  └ type = 0x10 (TCP_OPEN)
 └ ver = 0
```

### 9.3 TCP_DATA (stream_id=1, payload "GET /")

```
00 12 00 01 00 05 47 45 54 20 2F
                  └────┬────┘
                       └ "GET /"
```

---

## 変更履歴

| 日付 | 版 | 変更点 |
|---|---|---|
| 2026-05-26 | v0 / draft | 初版作成。Phase A 実装合意ドキュメント |
