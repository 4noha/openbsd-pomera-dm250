<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 4noha
-->

# BUILD-FROM-SOURCE — Release artifact を自分で再現する

Release 配布の `bsd.armv7.delay-2s` を **ソースから自分でビルドし直す**ための
要約。 詳細手順・トラブル例は [`../docs/cross-build-kernel.md`](../docs/cross-build-kernel.md)
側にあるので、 ここはオーバービューと「同じ SHA256 を出すための要点」だけ。

> [!IMPORTANT]
> pomera DM250 (RK3128 単一コア) で **実機 native build はやらない**。 73 分
> 以上かかり build 負荷で箱がクラッシュする。 **arm64 OpenBSD qemu VM + clang-22
> のクロスビルド一択**。

## 経路の選択

| ルート | 所要 | 用途 |
| --- | --- | --- |
| A. Release から prebuilt を取得 | 5 分 | 普通はこれ ([`README.md`](README.md)) |
| B. pomera 実機 native build | 73 分 + α | **やらない**（box クラッシュ） |
| C. arm64 OpenBSD VM で cross-build | 5〜6 分 | 本ドキュメントの対象 |

## 前提

- ホスト Mac (Apple Silicon) + qemu + HVF アクセラレーション。
- 上に OpenBSD/arm64 7.9 の VM が `builder@localhost:2222` で SSH 待受。
  `doas permit nopass :wheel`。 VM 構築は [`../docs/build-vm.md`](../docs/build-vm.md)。
- VM 内に `llvm-22` パッケージが入っている (`clang-22`, `llvm-ar-22`,
  `llvm-nm-22`, `llvm-strip-22` が `/usr/local/bin/`)。
- VM 内 `/usr/src` に **`jcs/openbsd-src` の `rk3128` ブランチ**が clone 済み。
  無ければ `git clone --depth 1 -b rk3128 https://github.com/jcs/openbsd-src.git /usr/src`。

## ビルド手順（要約）

実行場所タグ:
- `# on mac` — Mac
- `# on builder-vm` — qemu の arm64 OpenBSD VM (`builder@localhost:2222`)

### 1. クロスコンパイラ wrapper（`~/bin/cc`）

OpenBSD kernel Makefile が呼ぶ `cc` を armv7 ターゲットの clang-22 に差し替える。

```sh
# on builder-vm
mkdir -p ~/bin
cat > ~/bin/cc <<'EOF'
#!/bin/sh
exec /usr/local/bin/clang-22 -target arm-unknown-openbsd7.9 \
  -Wno-uninitialized-const-pointer "$@" -Wno-error
EOF
chmod +x ~/bin/cc
```

`-Wno-uninitialized-const-pointer` / `-Wno-error` は clang-22 が base clang(19)
より厳しく、 `sys/arch/armv7/exynos/crosec.c:90` で `-Werror` に落ちるのを
回避するため（DM250 と無関係な exynos ドライバなので警告昇格を抑える）。

### 2. clang-22 クロス専用 前処理パッチ

[`../kernel-patches/bus_space_notimpl-align-clang22.patch`](../kernel-patches/)
を当てる。 `sys/arch/arm/arm/bus_space_notimpl.S` の `NOT_IMPL` マクロ末尾
`.align 0` → `.align 2`。 機能無関係、 clang-22 必須の前処理。

```sh
# on builder-vm
cd /usr/src
patch -p0 < ~/patches/bus_space_notimpl-align-clang22.patch
```

> ルート B (実機 native) では base clang(19) なのでこのパッチは不要。

### 3. 機能パッチ集

[`../kernel-patches/`](../kernel-patches/) の以下を `/usr/src` に適用。

| Patch | 対象 | 役割 | 必須? |
| --- | --- | --- | --- |
| `bcmbt-delay-2s.patch` | `sys/dev/fdt/bcmbt_fdt.c` | post-firmware HCI reset 前 delay 250ms → 2s | BT 使うなら必須 |
| `dwmmc-resume-pwrseq.patch` | `sys/dev/fdt/dwmmc.c` | resume で SDIO(WiFi) chip を pwrseq power-cycle | 部分的 |
| `bwfm-sdio-resume-guard.patch` | `sys/dev/sdmmc/if_bwfm_sdio.c` | resume 再 attach レースの NULL ガード + detach barrier | 部分的 |

> [!CAUTION]
> **WiFi-resume 系（`dwmmc-resume-pwrseq` + `bwfm-sdio-resume-guard`）は根治
> していない**。 error 60 → task NULL 参照、 までは潰したがクラッシュは
> 後段に移っただけで残る。 現運用は「WiFi 活かすなら蓋を閉じない / BT-tether
> 時は WiFi off」で回避。 BT の `bcmbt-delay-2s` だけが必須・安定。
> WiFi-resume を追わない構成なら 2 本は外しても可（その場合は再現される
> binary が `bsd.armv7.delay-2s` と byte 一致しないので別名で扱う）。

```sh
# on builder-vm
cd /usr/src
patch -p0 < ~/patches/bcmbt-delay-2s.patch
patch -p0 < ~/patches/dwmmc-resume-pwrseq.patch
patch -p0 < ~/patches/bwfm-sdio-resume-guard.patch
```

### 4. ビルド

```sh
# on builder-vm
export PATH=$HOME/bin:$PATH        # ~/bin/cc (clang-22 wrapper) を最優先
export BSDOBJDIR=/home/builder/usrobj
cd /usr/src/sys/arch/armv7/conf
doas config GENERIC
cd /usr/src/sys/arch/armv7/compile/GENERIC
make obj
make
```

- 所要 5〜6 分。
- 成果物: `/usr/src/sys/arch/armv7/compile/GENERIC/obj/bsd`（armv7 ELF, ~7.4MB）。

### 5. 取り出し → pomera デプロイ

```sh
# on mac
scp -P 2222 \
  -o UserKnownHostsFile=$HOME/.ssh/build79-known_hosts \
  builder@localhost:/usr/src/sys/arch/armv7/compile/GENERIC/obj/bsd /tmp/bsd.armv7.new

scp /tmp/bsd.armv7.new <your-pomera-user>@<pomera-host>:/tmp/
```

```sh
# on pomera (installed)
doas mv /bsd /bsd.before-rebuild
doas install -m 0700 -o root -g wheel /tmp/bsd.armv7.new /bsd
doas reboot
```

## 同じ SHA256 を出すために

`bsd.armv7.delay-2s` (`1aa585ec…`) と **byte 完全一致**を狙うなら以下を揃える:

1. ベース commit: `jcs/openbsd-src` の `rk3128` ブランチ、 オリジナル
   `bsd.armv7.jcs-original` (`dd5c9394…`) を出した commit と同一。
2. クロスコンパイラ: clang-22（base clang 19 でも同 commit でビルドできるが
   final binary の byte 一致は保証されない）。
3. 機能パッチ: `bcmbt-delay-2s.patch` + `dwmmc-resume-pwrseq.patch` +
   `bwfm-sdio-resume-guard.patch` の 3 本セット（上述 §3 と同じ）。
4. 前処理パッチ: `bus_space_notimpl-align-clang22.patch`（clang-22 必須）。
5. config: `GENERIC` （カスタム config は使わない）。

ベース commit を引き上げる / 機能パッチを抜く / clang を 19 にする、 の
いずれをやっても SHA は変わる。 「同じ機能だけ欲しい」のであれば SHA 一致は
要件ではないが、 reproducible build を主張するなら 1〜5 全部を固定する。

## 詳細

- VM セットアップ: [`../docs/build-vm.md`](../docs/build-vm.md)
- クロスビルド全文: [`../docs/cross-build-kernel.md`](../docs/cross-build-kernel.md)
- パッチ本体と patch level の注意: [`../kernel-patches/README.md`](../kernel-patches/)

## License

- 本ドキュメントは MIT (`../LICENSE`)。
- ビルドした kernel binary は OpenBSD ソース由来で ISC 継承 (`../LICENSE-ISC`)。
- kernel patches は ISC 継承（パッチヘッダに記載）。
