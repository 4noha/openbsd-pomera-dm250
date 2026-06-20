<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# harness/mlterm-fb-patches/ — DM250 framebuffer console fixes for mlterm 3.8.3

DM250 (OpenBSD/armv7、ports `x11/mlterm` 3.8.3) の **framebuffer backend**
を実機で常用するために当てている 3 本の patch series。`harness/mlterm-main`
と `harness/mlterm-aafont` config と組で使う。

X は起動しないので、 console (`/dev/tty0`) で直接 framebuffer に描画する
`mlterm-fb` が唯一の i18n + emoji 端末。stock の `mlterm-fb` だと
NotoColorEmoji ロード時にゼロ除算で死ぬ・縦メトリックが潰れる・絵文字が
セル境界を踏み外す等で常用に堪えないため、これらを当てる。

## 何を直すか

| patch | 行 | 直すもの |
|---|--:|---|
| [`mlterm-fb-emoji-color-div0.patch`](mlterm-fb-emoji-color-div0.patch) | 2783 | `load_ft()` の `face->units_per_EM == 0` ガード (NotoColorEmoji 等のビットマップ専用フォントでゼロ除算 → SIGFPE クラッシュ) |
| [`mlterm-fb-dm250-display.patch`](mlterm-fb-dm250-display.patch) | 15487 | (1) 32 bpp framebuffer での shadow buffer + 1 回 memcpy blit (フォントサイズ変更・スクロールでハング/ちらつき) / (2) emoji グリフのセル中央配置とアスペクト維持縮小 (絵文字の半分欠け) / (3) Nerd Font アイコンのセル幅収まり (wifi/BT 等アイコンが見切れる) |
| [`mlterm-fb-decset2026-sync.patch`](mlterm-fb-decset2026-sync.patch) | 4836 | DECSET 2026 (synchronized output, BSU/ESU) サポート + 150 ms タイムアウト安全策。tmux ベースの Claude セッション表示でちらつきゼロ。タイムアウト無しだと stray BSU で画面凍結事故が起きるので必須 |

## 前提

`~/.mlterm/main` に以下が入っていること (`harness/mlterm-main` をそのまま
使えば揃う):

```
use_aafont = true
col_size_of_width_a = 1
unicode_full_width = false
unicode_full_width_areas = U+1F000-1FAFF
```

`use_aafont = true` + `col_size_of_width_a = 1` で Nerd Font の wifi/BT
アイコンを 1 セル (zsh の `wcwidth` と一致) に、 `unicode_full_width_areas`
で emoji を強制 2 セルにする。これが揃っていないと patch を当てても
表示が崩れる。

## ビルド手順

mlterm を ports からソース取得 → 3 patches を `-p1` で当てる → トップで
`gmake`。出力は `main/.libs/mlterm-fb` (libtool 実体)。setuid root の必要は
無く 755 で起動できる (`doas` 経由なら setuid 不要)。

```sh
# on pomera (installed) — pkg ソース取得経路
doas pkg_add -i mlterm        # まず stock 版を入れて build deps を引っ張る
cd /tmp
ftp https://ftp.openbsd.org/pub/OpenBSD/$(uname -r)/ports.tar.gz
tar xzf ports.tar.gz
cd ports/x11/mlterm
SUDO=doas make extract
cd /usr/ports/pobj/mlterm-3.8.3/mlterm-3.8.3   # extract 先

# 当てる順序: emoji-color-div0 → dm250-display → decset2026-sync
for p in /path/to/harness/mlterm-fb-patches/mlterm-fb-emoji-color-div0.patch \
         /path/to/harness/mlterm-fb-patches/mlterm-fb-dm250-display.patch \
         /path/to/harness/mlterm-fb-patches/mlterm-fb-decset2026-sync.patch; do
    patch -p1 < "$p"
done

# 設定 (X は不要、 fb のみ)
./configure --without-x --enable-utmp --with-imagelib=none
gmake -j2

# 出力確認: DECSET 2026 シンボルが入ったか
nm main/.libs/mlterm-fb 2>/dev/null | grep mlterm_fb_sync
# → mlterm_fb_sync が B (BSS) で出れば OK
```

## デプロイ

> [!CAUTION]
> 稼働中の `mlterm-fb` プロセスを `cp -f` で上書きすると **走っている
> プロセスの text セグメントが書き換わる** 可能性がある (OpenBSD は
> ETXTBSY を返さないケースがある)。 必ず `install -m 0755` か
> `mv` (atomic rename = 新 inode) で配置すること。

```sh
# on pomera (installed)
doas install -o root -g bin -m 0755 main/.libs/mlterm-fb /usr/local/bin/mlterm-fb

# config (まだなら harness/ からコピー)
mkdir -p ~/.mlterm
cp /path/to/harness/mlterm-main   ~/.mlterm/main
cp /path/to/harness/mlterm-aafont ~/.mlterm/aafont
```

停止は `SIGTERM` で clean exit させること (`SIGKILL` だと wscons の VT
state が汚れて getty が見えなくなる。詰んだら `doas reboot` でリセット)。

> [!NOTE]
> **OSC 5379 のランタイムフォントリサイズは使わない**。`printf '\e]5379;
> fontsize=N\a'` で mlterm-fb がデバイスを落として再起動する挙動があり、
> これが ssh セッションの cleanup/EXIT 内で叩かれると、走っている
> parent (ssh / claude wrapper) が終わらなくなる事故が出る。font 切替が
> 要るならセッションを切ってから `~/.mlterm/main` の `fontsize=` を書き
> 換えて再起動する。`tailscale-optional/claude` の `set_font()` は
> このため no-op になっている (2026-06-12)。

> [!TIP]
> **256 色を確実に通す**: tmux 越しに 256 色が抜けない場合は mlterm の
> `~/.mlterm/main` で `termtype = xterm-256color` を明示する。default の
> `xterm` (terminfo の "xterm" は 8 色エントリ) が tmux 側を 8/16 色に
> 絞っているのが真因。`xterm-256color` の terminfo は macOS 標準 +
> OpenBSD base に両方入っているので、ssh で往復してもズレない。

## 検証 (実機実測)

| 観察 | 期待 |
|---|---|
| ` 😀😃😄😁` を `cat` で流す | カラーで表示、クラッシュなし |
|  (Nerd Font wifi icon) 等 | セル幅に収まって左右切れない |
| フォントサイズ変更 (Ctrl + Shift + + / -) | ハングせず、ちらつかない |
| `tmux` 越しに高頻度 redraw (claude-master 等) | BSU/ESU で 1 frame 原子的に出る、ちらつきゼロ |
| BSU 後 ESU が来ないままセッション切断 | 150 ms タイムアウトで強制 blit、画面凍結しない |

## 関連

- `harness/mlterm-main` / `harness/mlterm-aafont` — patch を当てた
  `mlterm-fb` が要求する `~/.mlterm/` 設定。
- `prebuilt-info/` — patch を当ててビルド済みの `mlterm-fb.armv7` を
  Releases asset として提供する場合のマニフェスト場所。
- 上流: <https://github.com/arakiken/mlterm> mlterm 3.8.3 (ports
  `x11/mlterm` 版)。
