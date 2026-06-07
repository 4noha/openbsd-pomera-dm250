# 05-pairing-runbook.md

pomera ↔ Android の BT ペアリング (新規 / 再ペア) を運用安全に行う手順。

## 設計上の前提

`panctl` は **secure-by-default** で動く。 接続待ち受け (discoverable) は
**operator が `panctlctl advertise on` を投げない限り無効**で、 ペアリング
コミットも **`panctlctl confirm` の人手承認** が要る。 これは家庭内・外出先で
勝手にペア要求されることを防ぐ意図的な設計。

そのため、 「Android で BT 設定開いて待つ」 だけでは pomera は見えない。
operator (= pomera ssh セッションを持つ人) が advertise on を投げる必要がある。

## 用語

| 略 | 意味 |
|---|---|
| **operator** | pomera に ssh して `panctlctl` を投げられる人 |
| **link key** | ペア成立で生成される共有秘密。 `/var/db/panctl/tlv.dat` に永続化 |
| **passkey** | ペア時に Android が表示する 6 桁数字。 operator が `panctlctl confirm` 投入時に visual verify |

## 0. 事前準備

- panctl と netwatchd が動いていること、 BT chip が attach 済 (`dmesg | grep bcmbt`)
- `panctl_flags` に `--android-bdaddr <ペアしたい Android の BD_ADDR>` が
  入っていること (なければ `doas rcctl set panctl flags "..."` で設定)
- Android 側 BT が ON

```bash
# on pomera (確認)
rcctl get panctl | grep flags
dmesg | grep bcmbt | tail -2
```

## 1. 既存ペアがあれば両側でクリア (再ペアの場合のみ)

### pomera 側

> [!IMPORTANT]
> **netwatchd を先に止める**。 netwatchd の panctl watchdog が即座に panctl を
> respawn するので、 そのままだと「panctl を止めても消えない」ように見える。

```bash
# on pomera
doas rcctl stop netwatchd                 # まず watchdog を黙らせる
doas pkill -f /usr/local/sbin/netwatchd.sh
doas rcctl stop panctl
doas pkill -f /usr/local/sbin/panctl

# 確認: 両方消えていること
ps -ax | grep -E "netwatchd|panctl" | grep -v grep

# stale link key を消す
doas rm -f /var/db/panctl/tlv.dat /tmp/.bt-state
```

> [!CAUTION]
> `doas rcctl disable panctl` を投げると **`panctl_flags` が空になる** ことが
> ある (2026-05-31 実機で観測)。 disable は使わず、 `stop` だけにすること。
> 万一空になったら以下で復元:
> ```
> doas rcctl set panctl flags "--android-bdaddr <BD_ADDR> -t h4 -d /dev/cua00 --udp-mode tun --tun-dev /dev/tun0"
> ```

### Android 側

Settings → Bluetooth → 「pomera」 (BD_ADDR `XX:XX:XX:XX:XX:XX`) があれば
「ペア解除」「忘れる」「Unpair」 で削除。 Bluetooth は OFF → ON でリフレッシュ
しておくと検出がきれい。

## 2. panctl 起動 + advertise on

```bash
# on pomera (operator)
doas rcctl start panctl
sleep 2
ps -ax | grep /usr/local/sbin/panctl | grep -v grep   # 起動確認

# discoverable に
doas /usr/local/sbin/panctlctl advertise on
# → "advertise on (discoverable)"

doas /usr/local/sbin/panctlctl status
# → "advertise on" / "idle"
```

これで pomera が BT scan に見えるようになる。

## 3. Android 側で pomera を選択

Android Bluetooth 設定 → 「ペアリングする」「Pair new device」:

1. デバイス一覧に `pomera` (BD_ADDR `XX:XX:XX:XX:XX:XX`) が出る
2. tap
3. **passkey 6 桁** が画面に出る (例: `314223`)

passkey を operator に共有する (口頭・チャット・Slack・付箋等、 secure channel
ならなんでも)。 attacker は同時刻に同じ passkey は生成できないので、
これが本人確認になる。

## 4. operator 側で passkey verify + confirm

passkey を visual で照合してから:

```bash
# on pomera (operator)
doas /usr/local/sbin/panctlctl confirm
# → "confirmed pair with <BD_ADDR> (passkey 314223)"

# ログで bonding 完了を確認
grep panctl /var/log/daemon | tail -10
# → "pairing complete status=0x00" が出ていれば成功
```

status=0x00 以外なら失敗。 代表的なエラー:
- `0x05`: passkey mismatch (Android の番号と異なる)
- `0x06`: PIN/Key missing (link key cleanup 漏れ)
- timeout: passkey 入力タイミングが過ぎた → やり直し

passkey 一致しなかったら **絶対に confirm せず**、 `panctlctl deny` で拒否。

## 5. 後片付け

```bash
# on pomera
doas /usr/local/sbin/panctlctl advertise off   # 余計な discoverable を切る
doas rcctl start netwatchd                      # watchdog 復帰
doas /usr/local/sbin/panctlctl status           # → "advertise off, idle"

# link key 保存確認
doas ls -la /var/db/panctl/tlv.dat              # → 96 bytes 程度
```

## 6. 動作確認 (Android アプリ側が用意できてから)

Android の mux protocol アプリを起動して RFCOMM channel をアドバタイズ:

```bash
# on pomera
grep panctl /var/log/daemon | tail -10
# 期待: "RFCOMM connected" / "mux open" 系のログに切り替わる
# 失敗: "SDP completed but no matching RFCOMM channel" のループ継続
#       (= Android 側 mux app が listen していない)

cat /tmp/.bt-state                              # → "up" になれば PS1 にも [bt] が出る
```

PS1 が `[wifi+bt]` (or `[bt]`) になれば end-to-end OK。

## トラブルシュート

### panctl が「bad bdaddr: atexit」で死ぬ

`panctl_flags` が空。 §1 の CAUTION ノート参照、 flags を再設定。

### Android に passkey ダイアログが出ない

- pomera 側 `panctlctl advertise on` を投げたか?
- Android Bluetooth 設定で「ペアリングする」モードに入ってるか?
- Bluetooth OFF → ON で refresh してみる

### passkey は出たが confirm 後も `pairing complete` が出ない

- panctl がクラッシュしてないか (`ps -ax | grep panctl`)
- 5 秒以上経っても出ないなら timeout、 `panctlctl status` で `idle` に戻ってる
  → 最初からやり直し (advertise off → tlv.dat 削除 → advertise on → ...)

### link key が再起動で消える

`/var/db/panctl/tlv.dat` のパーミッション・所有者が壊れてる可能性。
`doas ls -la /var/db/panctl/` で確認、 `root:wheel` `drwx------` が正常。

## セキュリティ姿勢

- **advertise on は ssh セッション中だけ短時間**。 operator が物理的に
  pomera の前に居ない / ssh 切断後 / 不要時はすぐ off
- **passkey verify を絶対省略しない**。 自動 confirm 化はしない (将来 panctl
  に `--auto-confirm` フラグが付いても本リポでは使わない)
- **link key は `/var/db/panctl/tlv.dat` に root:wheel 600 で保持**。
  バックアップする場合も同じパーミッションを維持
- **mass-pair 防止**: 1 つの BD_ADDR に紐付ける運用 (`--android-bdaddr` で
  指定された 1 機種だけ正規)。 別 Android を使うときは link key 入れ替え
  (= 本手順を最初から)
