# 04 - pomera 側 outbound intercept (pf divert vs tun) 検証設計

[DESIGN.md](DESIGN.md) Phase A Step 6 の **「全アウトバウンド TCP/UDP を userland の mux client に持ち上げる」**方式の比較・検証メモ。

実機テストはまだ行えないので、**「どちらを基本にするか」「どう試して判定するか」「失敗の見分け方」**を文書化する。実機で実測したら結果をここに追記する。

## 1. 候補 2 つ

### 1-A. `pf divert-to` ベース

pf で全アウトバウンドを **同一 host 上の userland socket** に redirect する。

```pf
# /etc/pf.conf (抜粋)
# 100.64.0.0/10 (Tailscale CGNAT 帯域) は除外。Tailscale 内 peer 宛は WireGuard 直
# RFCOMM は loopback なのでそもそも pf を通らない

set skip on lo

# panctl が listen するソケットへ divert
pass out inet proto { tcp udp } from any to !100.64.0.0/10 \
    divert-to 127.0.0.1 port 9999
```

panctl 側:

```c
int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
struct sockaddr_in sin = { .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY,
                           .sin_port = htons(9999) };
bind(s, (struct sockaddr*)&sin, sizeof sin);
listen(s, 64);
// accept() で来る fd は元の dst_addr/dst_port を `getsockname(originaldst)` で
// 取れる (OpenBSD 拡張: getsockname に PF_DIVERT socket flag が立つと original
// destination が返る)
```

UDP は `divert-packet` + `SOCK_RAW` で受ける必要があり、もう少し低レベル。

**長所**:
- panctl は `accept()` / `recvfrom()` だけでアプリ層の socket として扱える
- TCP は普通の stream socket、kernel が SYN/ACK/RST のハンドリングを終えた後に届く
- pf でルールを書くだけなので、対象範囲の柔軟性 (ホスト/ポート/インタフェース別) が高い

**短所**:
- OpenBSD 固有。Linux ポータビリティ皆無 (ただし panctl は OpenBSD only でいい)
- **UDP の取り扱いが TCP より厄介**: `divert-to` は TCP 主用途。UDP は `divert-packet` で raw に受けて自分で reply paths を書く必要がある (Tailscale の WireGuard UDP がそれで通るか要検証)
- divert socket は default ルートテーブルに依存。Tailscale が `tun0` を生やしている時の挙動 (routing table 1 を使うケース) は要確認

### 1-B. `tun(4)` をデフォルトルートにする

pomera 側で `tun0` を生やし、デフォルトルートを `tun0` に向ける。panctl は `tun0` から **生 IP datagram** を read し、L4 ヘッダ (TCP/UDP) だけ見て stream に振り分ける。

```sh
# on openbsd
doas ifconfig tun0 create
doas ifconfig tun0 inet 169.254.42.1/32  # local-side dummy
doas route add default 169.254.42.1
# panctl が /dev/tun0 を open し、tun frame を read
```

panctl 側 (擬似):
```c
int fd = open("/dev/tun0", O_RDWR);
uint8_t pkt[2048];
while ((n = read(fd, pkt, sizeof pkt)) > 0) {
    // pkt[0]: IPv4 ヘッダ。proto field を見て:
    //   TCP (6)   → src/dst を見て TCP_OPEN / TCP_DATA に変換 (要 mini TCP state)
    //   UDP (17)  → UDP_PACKET に変換
    //   その他    → drop or log
}
```

**長所**:
- ポータブル (Linux でも同じ tun frame 構造)
- TCP/UDP の境目を panctl で完全に持つので、UDP の流量制御も自前

**短所**:
- panctl 側に **mini TCP state machine が要る** (3-way handshake を panctl から開始する形になる)。1-G の「L4 forwarding のみ、TCP state machine は持たない」原則と矛盾
- 失敗すると 1-H に半分降りた状態になる

### 結論方針: **divert-to を本命、tun(4) を保険**

DESIGN.md §2 の「Android 側に IP stack を持たない」「pomera 側 L4 forwarding のみ」を最大化するため、**`pf divert-to` ベースを Phase A Step 6 の本命**とする。tun(4) は 1-H 退避案に半分入っているので、divert で詰まったら 1-H 全面採用への分岐点として扱う。

## 2. 検証手順 (Phase A Step 6 で実機実施)

### Step 6.0 準備

- amd64 OpenBSD で panctl が RFCOMM 接続できている (Step 5 完了)
- mux protocol の echo / TCP open テストが通っている (Step 5 完了)
- pf 既定設定が `pass all` (sanity)

### Step 6.1 TCP divert の疎通

最小実験。panctl の TCP accept loop を有効化し、pf で 1 ポートだけ divert する:

```pf
pass out inet proto tcp from any to any port 80 \
    divert-to 127.0.0.1 port 9999
```

```sh
# on openbsd
$ curl http://example.com/
```

期待挙動:
- panctl の TCP accept() が走り、`getsockname(SOL_DIVERT, ...)` で original dst が `93.184.215.14:80` 相当に取れる
- mux 経由で Android 側に TCP_OPEN を投げ、200 OK の HTML が返る
- `curl` 側に HTML が表示される

失敗パターンと切り分け:
| 症状 | 原因 | 対処 |
|---|---|---|
| `accept()` に来ない | pf ルール未適用 / pfctl -e 忘れ | `pfctl -sn`, `pfctl -sa` で active rules 確認 |
| `accept()` には来るが getsockname で original dst が `127.0.0.1:9999` のまま | OpenBSD のバージョンによる divert socket option 差 | `setsockopt(s, IPPROTO_IP, IP_DIVERTFL, ...)` 系を再確認 |
| Android 側に TCP_OPEN は届くが応答が無い | Android アプリの mux server バグ | アプリ側のログを確認 |

### Step 6.2 UDP divert の疎通

UDP は `divert-packet` ベース。

```pf
pass out inet proto udp from any to any port 53 \
    divert-packet port 9999
```

```c
int s = socket(AF_INET, SOCK_RAW, IPPROTO_DIVERT);
struct sockaddr_in sin = { .sin_family = AF_INET, .sin_port = htons(9999) };
bind(s, (struct sockaddr*)&sin, sizeof sin);
// recvfrom() で生 IP packet と divert info が取れる
```

```sh
# on openbsd
$ dig @1.1.1.1 example.com
```

期待: panctl が recvfrom で IPv4+UDP raw packet を取り、`UDP_BIND` + `UDP_PACKET` を mux に流し、Android 側で 1.1.1.1:53 へ実 socket で送出、戻り datagram を mux 経由で返す。

成功判定: `dig` が応答を取れる (timeout しない)。

### Step 6.3 Tailscale 起動

ここまで通ったら Tailscale を入れる。

```sh
# on openbsd
$ doas pkg_add tailscale
$ doas tailscale up --hostname=pomera-dm250
```

`tailscale up` は:
1. `controlplane.tailscale.com` を DNS で引く → unbound (DoT) 経由で解決
2. controlplane に HTTPS で接続 → mux 経由
3. WireGuard ピア情報を受け取る
4. WireGuard UDP (50000+ ephemeral) でピアにハンドシェイク → mux 経由

成功判定: `tailscale status` で peer が `connected (direct)` または `connected (relay)` になる。

`relay` (DERP fallback) で十分実用なら本リリースとして OK。`direct` を狙うと UDP NAT 越えが必要だが、mux 経由でも src_port が固定維持されれば成立する。実測してから判断。

### Step 6.4 ssh + tmux 試験

```sh
# on openbsd
$ ssh home.tailnet.ts.net  # ホスト名は実環境による
$ tmux attach -t claude-master
```

ターミナル操作の応答性 (~100ms 程度の RTT) が確保できれば Phase A 完了。

## 3. 判定基準

### Phase A Step 6 → Step 7 進行基準

| 項目 | 必須 | 推奨 |
|---|---|---|
| `curl http://example.com` 成功 | ✓ | |
| `dig @1.1.1.1 example.com` 成功 | ✓ | |
| `tailscale up` 成功 | ✓ | |
| Tailscale が direct UDP path | | ✓ (relay でも可) |
| ssh + tmux 100ms 以下の応答 | | ✓ |

### 1-H への降伏判定 (これが起きたら tun(4) ベースに転換)

| 兆候 | 解釈 |
|---|---|
| divert-packet で UDP がそもそも届かない (pf ルール正しいのに) | OpenBSD カーネル側の divert subsystem の limit を踏んでいる |
| UDP 戻り datagram が NAT で迷子になる | divert で reply path を再構築する logic が脆い → tun(4) ベースなら IP header を panctl で生成して再注入できる |
| Tailscale WireGuard が relay 含めて到達不能 | mux UDP 経路の MTU / fragmentation 問題 → tun ベースで MTU を絞れる |

降伏した場合は本ドキュメント末尾に **「降伏した日 / 原因 / 観察ログ」** を追記し、tun ベースの実装を `panctl/divert.c` の代わりに `panctl/tun_intercept.c` として書き直す。

## 4. 周辺事項

### 4.1 Tailscale 除外

CGNAT (`100.64.0.0/10`) と `fd7a:115c:a1e0::/48` は Tailscale が握る帯域。divert ルールでは **必ず除外**する。Tailscale 起動後の WireGuard UDP は loopback ではないので除外しないとループする。

### 4.2 ICMP

ICMP は v0 では mux に流さない (1-G の非目的)。pf で `pass out inet proto icmp` を **block** しておき、`ping` は panctl 内 ICMP echo は実装しない (将来必要なら mux に ICMP frame type を追加)。

### 4.3 起動順序

```
1. boot
2. /etc/rc.d/panctl  ← RFCOMM 接続が確立、mux ready
3. /etc/rc.d/unbound ← DoT で名前解決可能
4. /etc/rc.d/tailscaled
```

panctl が落ちると Tailscale も DNS も死ぬので、panctl の **再接続ループは最優先**で堅牢にする。`rc.d` レベルでは `check` で再起動を担保。

### 4.4 ループバック例外

`pass in / out on lo0` を `set skip on lo` で divert 対象から外す。divert が loopback まで掴むと panctl 自身の HCI ↔ libusb 通信 (loopback ではないが、divert socket の入出力) が grease する。

## 5. 実機ログを書く欄 (Phase A Step 6 実施時に埋める)

実機テスト後ここに以下を追記する:

- 実施日、OpenBSD バージョン、panctl revision
- `pfctl -sa` の active rules
- TCP / UDP / Tailscale それぞれの疎通結果 (成功 / 失敗 + ログ抜粋)
- 採用方式の最終決定 (divert-to / tun(4))
- 1-H への降伏判定が起きた場合は別ドキュメント (`05-tun-intercept.md`) を追加

> 現在: 未実施
