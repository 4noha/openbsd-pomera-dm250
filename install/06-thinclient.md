<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# 06. シンクライアント本番接続 (panctl + Tailscale + netwatchd)

[`05-post-install.md`](05-post-install.md) で OS 基本セットアップが
終わった状態から、pomera を **Tailscale 越しに自宅 Mac の tmux に
ssh attach する thin-client** に仕立てる。

## 6.1 panctl 配布物の build + install

mux ベースの BT-tether daemon `panctl` を pomera native build → permanent
install。詳細は [`../panctl/`](../panctl/) の README。

```sh
# on pomera (installed)
# 前提: gmake + git + clang (base install 含む)
doas pkg_add git gmake
cd ~
git clone --depth 1 --branch v1.6.2 https://github.com/bluekitchen/btstack.git
cd btstack
# patches は ../panctl/patches/ に置いてある想定
patch -p1 < ~/panctl/patches/btstack-v1.6.2-openbsd-compat.patch
patch -p1 -d port/posix-h4 < ~/panctl/patches/btstack-config-disable-le-secure-for-pair-stability.patch

# panctl src + tools/panctlctl.c をリポから配置
mkdir -p ~/panctl-build/{panctl,tools}
cp ../panctl/src/*.c ~/panctl-build/panctl/
cp ../panctl/src/*.h ~/panctl-build/panctl/
cp ../panctl/tools/panctlctl.c ~/panctl-build/tools/
cp ../panctl/build_panctl.sh ~/

# build (約 5 分)
TOOLS=~/panctl-build/tools ~/build_panctl.sh
# → /tmp/panctl (約 580KB), /tmp/panctlctl (約 7KB)

# permanent install
doas install -o root -g bin -m 0755 /tmp/panctl    /usr/local/sbin/panctl
doas install -o root -g bin -m 0755 /tmp/panctlctl /usr/local/sbin/panctlctl
doas install -o root -g wheel -m 0700 -d /var/db/panctl
doas install -o root -g wheel -m 0755 -d /var/run/panctl

# rc.d script
doas install -o root -g bin -m 0755 \
    ../panctl/etc/rc.d/panctl /etc/rc.d/panctl
```

> [!NOTE]
> 上記の相対パス `../panctl/` は **このリポの top-level `panctl/`** ディレクトリ
> から見たもの。pomera 上で展開した先に合わせて読み替えること。

## 6.2 BT pair (Just Works + LE Secure 無効化)

`btstack_config.h` で `ENABLE_LE_SECURE_CONNECTIONS` /
`ENABLE_CROSS_TRANSPORT_KEY_DERIVATION` を **無効** にした build を使うこと
(§6.1 で patch 適用済)。これを忘れると `pairing complete status=0x05`
(HCI Authentication Failure) で詰む。経緯は [`../panctl/`](../panctl/)
README 参照。

```sh
# on pomera (installed) — まず panctl flags 設定後 (§6.3 参照) 自動起動
# Android 側で MuxServer 起動 (BtConnectReceiver で自動でも可)
# pomera 側で discoverable を一時 ON
doas /usr/local/sbin/panctlctl advertise on

# Android Settings → Bluetooth → スキャン → pomera-panctl タップ
# → ペアダイアログ → [ペア] タップ → 完了 (pomera 側は auto_accept)
# 終わったら閉じる
doas /usr/local/sbin/panctlctl advertise off
```

`/var/db/panctl/tlv.dat` に link key 永続化、以降は reboot 後も自動再接続。

## 6.3 tun0 + 静的ルーティング + rc.conf.local

```sh
# on pomera (installed)
doas tee /etc/hostname.tun0 <<'EOF'
!ifconfig tun0 create up
inet 10.66.66.2 255.255.255.0 10.66.66.1
mtu 1000
EOF

# rc.conf.local に panctl 起動 flags
ANDROID_BDADDR=XX:XX:XX:XX:XX:XX   # 自分の Android の BT MAC
echo "panctl_flags=\"--android-bdaddr $ANDROID_BDADDR -t h4 -d /dev/cua00 --udp-mode tun --tun-dev /dev/tun0\"" | doas tee -a /etc/rc.conf.local

doas rcctl enable panctl
doas rcctl start  panctl
```

## 6.4 Tailscale 加入

詳細は [`../tailscale-optional/`](../tailscale-optional/)。

```sh
# on pomera (installed)
doas pkg_add tailscale
doas rcctl enable tailscaled
doas rcctl start  tailscaled

# 初回のみ手動で auth (URL が表示される、Mac ブラウザで開いて承認)
doas tailscale up --hostname=<pomera-host> --timeout=60s
doas tailscale ip   # → 100.x.x.x が確定
```

## 6.5 default route 切替 (mux 経由 internet にする時)

```sh
# on pomera (installed)
doas route delete default
doas route add default 10.66.66.1
# 戻す時:
# doas route delete default && doas route add default <your-lan-gw>
```

恒久化したい場合は `/etc/hostname.bwfm0` の `inet autoconf` を
`inet <dm250-lan-ip> 255.255.255.0 NONE` 等にして dhclient の default route
設定を切る + `/etc/mygate` で 10.66.66.1 指定。 ただし boot 順序問題ある
(tun0 が panctl 起動前に default にできない) ので **default は bwfm0 に残し、
必要時 manual で tun0 に switch** する運用が安定。

WiFi → BT default route failover の自動化は §6.7 の netwatchd に統合する
運用にしてもよい (詳細は `../tailscale-optional/` 配下の運用メモ参照)。

## 6.6 複数 WiFi AP 登録 + LAN 跨ぎ対応

持ち運び時に複数の WiFi AP を使う場合、`/etc/hostname.bwfm0` に `join`
行を複数並べると **電波の強い方に自動接続**する。

```sh
# on pomera (installed)
doas tee /etc/hostname.bwfm0 > /dev/null <<'EOF'
join AP1_SSID wpakey AP1_PASS
join AP2_SSID wpakey AP2_PASS
inet autoconf
EOF
doas sh /etc/netstart bwfm0
```

接続先 AP が変わると **LAN subnet も変わる** ので、Mac 側で固定 IP で
ssh していると届かなくなる。これは §6.4 の Tailscale magic DNS を使えば
解決する (`<pomera-host>.<your-tailnet>.ts.net` は subnet 越えても同じ
name で引ける)。

Mac 側 `~/.ssh/config` に alias を作っておくと便利:

```sh
# on mac
mkdir -p ~/.ssh && chmod 700 ~/.ssh
cat >> ~/.ssh/config <<'EOF'

Host pomera
    HostName <pomera-host>.<your-tailnet>.ts.net
    User <your-pomera-user>
EOF
chmod 600 ~/.ssh/config

# 初回接続時に host key を信頼
ssh-keyscan -t ed25519 <pomera-host>.<your-tailnet>.ts.net >> ~/.ssh/known_hosts

# 以降は ssh pomera で一発
ssh pomera
```

## 6.7 netwatchd: resume + Wi-Fi 自動再接続 + panctl 監視

蓋を閉じた後 (suspend → resume) や AP 圏外移動で Wi-Fi association が切れた
ときに、ユーザ操作無しで再接続する常駐スクリプト。panctl (BT) も自動再起動。

- 実装: `/usr/local/sbin/netwatchd.sh` (約 80 行の sh)
- rc: `/etc/rc.d/netwatchd` (nohup detach 起動)
- 動作原理:
  - 5 秒 poll、`date +%s` の差分が 10 秒超 → resume と判定 →
    `sh /etc/netstart bwfm0` + `rcctl restart panctl`
  - Wi-Fi link が `status: active` でない状態が 15 秒続いたら
    `sh /etc/netstart bwfm0` で再 join (AP 切替もこれで)
  - panctl が pgrep で見つからなければ `rcctl start panctl`

```sh
# on mac (Tailscale 経由デプロイ)
scp install/files/netwatchd.sh   pomera:/tmp/
scp install/files/netwatchd.rc.d pomera:/tmp/

# on pomera (installed)
doas install -m 0755 /tmp/netwatchd.sh   /usr/local/sbin/netwatchd.sh
doas install -m 0555 /tmp/netwatchd.rc.d /etc/rc.d/netwatchd
doas rcctl enable netwatchd
doas rcctl start  netwatchd

# 確認
rcctl check netwatchd                       # → netwatchd(ok)
grep netwatchd /var/log/daemon | tail -5
```

設定を変えたい場合は `/etc/netwatchd.conf` を作って source される変数を
上書き (`POLL`, `RESUME_THRESHOLD`, `WIFI_DOWN_RETRY`, `PANCTL_ENABLED` 等)。

## 6.8 動作確認チェックリスト

```sh
# on pomera (installed)
doas rcctl check panctl tailscaled        # → 両方 (ok)
doas tailscale ip                          # → 100.x.x.x
ifconfig tun0 | grep -E "active|10.66"     # → status: active, inet 10.66.66.2

# BT mux 動作
doas tail -5 /var/log/daemon | grep -E "peer HELLO|RFCOMM open"
# → "RFCOMM open cid=1 mtu=990" と "peer HELLO" があれば mux 確立

# default を tun に切替えて curl
doas route delete default && doas route add default 10.66.66.1
curl -s -m 8 -o /dev/null -w "HTTP %{http_code}\n" http://example.com/
# → HTTP 200 (mux 経由 cellular で example.com 到達)

# Tailscale 経路で home Mac に ssh
ssh claude "hostname"
# → <home-mac> の hostname
```

WiFi 完全切断テスト (`ifconfig bwfm0 down` → curl → `bwfm0 up`) は
[`../panctl/`](../panctl/) の "BT-only 動作確認" セクション参照。

## 6.9 起動高速化 (任意・低リスク)

シンクライアント用途で不要なサービスを止め、FFS に `softdep` を付けると起動と
ディスク I/O が軽くなる。実機計測 (RK3126 単核 Cortex-A7 1.2GHz):
cold reboot → ログインプロンプト ≈ +154s、Tailscale ssh 到達 ≈ +146s。

### 不要サービスの無効化 (戻し: `doas rcctl enable <名前>`)

```sh
# on pomera (installed)
doas rcctl disable smtpd        # メール (thin client では不要)
doas rcctl disable sndiod       # オーディオ
doas rcctl disable check_quotas # quota 未使用なら boot 時 no-op
doas rcctl stop smtpd sndiod    # 今すぐ止める (check_quotas は boot 専用なので stop 不要)
```

> [!CAUTION]
> `ntpd` / `slaacd` / `dhcpleased` / `tailscaled` / `panctl` は **触らない**。
> 時刻は TLS/Tailscale の生命線、ネットワークは本機の存在意義。

### FFS に softdep (次回 boot で有効)

`/etc/fstab` の各 ffs 行の options に `softdep` を足す。**このシステムの
fstab は DUID 表記** (`xxxxxxxxxxxxxxxx.a` 等) なので device 名は触らず
options だけ追記する。

```sh
# on pomera (installed)
doas cp -p /etc/fstab /etc/fstab.bak-softdep        # backup 必須
# ffs 行の 4 列目に softdep を追記 (既にあればスキップ)
awk 'BEGIN{OFS=" "} /^[[:space:]]*#/{print;next} NF==0{print;next} \
     ($3=="ffs" && $4 !~ /softdep/){$4=$4",softdep"} {print}' /etc/fstab > /tmp/fstab.new
# 「softdep を剥がすと元と一致」= softdep 以外いじってない、を確認してから差し替え
diff <(awk '{$1=$1;print}' /etc/fstab) <(sed 's/,softdep//g' /tmp/fstab.new | awk '{$1=$1;print}') \
  && doas cp /tmp/fstab.new /etc/fstab && echo "fstab UPDATED"
```

結果 (例):
```
xxxxxxxxxxxxxxxx.a /       ffs rw,softdep 1 1
xxxxxxxxxxxxxxxx.e /home   ffs rw,nodev,nosuid,softdep 1 2
xxxxxxxxxxxxxxxx.d /usr    ffs rw,wxallowed,nodev,softdep 1 2
yyyyyyyyyyyyyyyy.a /mnt/sd ffs rw,nodev,nosuid,noauto,softdep 0 0
```

- **復旧**: softdep で起動に問題が出たらシングルユーザ
  ([`07-recovery.md`](07-recovery.md) の「FFS 不整合」) で
  `cp /etc/fstab.bak-softdep /etc/fstab`。
- `diff <(...)` は ksh/zsh の process substitution。`sh` で流す場合は
  一時ファイルで代替。

## ここまで終わったら

電源 ON → 数十秒で `ssh claude` で home tmux に attach する thin-client
状態が完成。

任意の追加要素:
- 起動ロゴ → [`../logo/`](../logo/)
- 電池/ネット PS1 → [`../battery/`](../battery/)
- suspend/resume の追い込み → [`../kernel-patches/`](../kernel-patches/) +
  [`../harness/`](../harness/)
- Tailscale 運用詳細 → [`../tailscale-optional/`](../tailscale-optional/)

運用上の宿題:
- `dhcpleased` の WiFi 復帰タイミング (一部は §6.7 の netwatchd でカバー)
- `tailscaled` の boot 時起動順 (panctl/tun0 まだ down 時に start すると失敗
  → rc.conf.local の依存順 or 起動遅延)
- BD_ADDR 揺れの固定化 (BTstack chipset hook)
- Numeric Comparison 復活 (BD_ADDR 固定 + LE Secure 再有効化セット)

壊れたら → [`07-recovery.md`](07-recovery.md)。
