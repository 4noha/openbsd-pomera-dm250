# battery/

DM250 の電池まわりの **userspace 作業場**。 PS1 表示、 計測、 観察ログ、
キャリブレーション、 OCV 補正のチューニング。

OpenBSD の `hw.sensors.simplebat0` は VBAT 端子電圧をそのまま `percent0` に
線形マップしているだけなので、 充放電中の電圧 sag/swell で SOC 表示が ±20pt
跳ねる。 ここでは ksh の PS1 関数で **OCV 補正 + 工場 OCV table 線形補間**を
かけて jump を ±1pt まで圧縮する。

## このディレクトリの守備範囲

- **PS1 `_bat` 関数の進化**を git 管理 (`~/.kshrc` の各時点 snapshot を `kshrc/`
  に保管)
- **実機観察ログ** (`-79 → +95 → -75` のような jump 計測、 残量帯ごとの誤差)
- **R / OCV table のキャリブレーション記録**

すべて Mac 上で完結する作業。 実機 ssh は計測時のみ。

## やらないこと

- **カーネルドライバの実装** — `kernel-patches/` の責務 (本物の RK818 fuel
  gauge ドライバ移植は jcs/openbsd-src 側の話)
- **電池そのもののリバースエンジニアリング** — RK818 + 4.2V/5800mAh Li-ion
  単セルという事実を信用する

## ライセンス境界 (clean-room)

このディレクトリの awk / ksh / C コードは **すべて MIT** (`SPDX-License-Identifier:
MIT` ヘッダ参照)。 Linux カーネル側の RK818 fuel gauge ドライバ
(`drivers/power/supply/rk818_battery.c`、 GPL-2.0) は **動作と数式の挙動を読み、
独立に書き直した**もので、 コード片の流用は一切していない。 OCV table と
`bat_res` は工場 dtb (King Jim 配布の `out/rk-kernel.dtb`) から抽出した数値
データで、 著作物に該当しない。 詳細は `../NOTICE` の "Linux RK818 reference"
節を参照。

## 関連サブツリー

| ディレクトリ | 関係 |
|---|---|
| [`../kernel-patches/`](../kernel-patches/) | **kernel 側**の同テーマ。 HW クーロンカウンタ経由の本物の fuel gauge ドライバ移植 (患者の RK818 driver patch を含む) |
| [`../logo/`](../logo/) | 工場 dtb (`out/rk-kernel.dtb`) を抽出するツール。 工場 OCV table 21 点と `bat_res=45mΩ` はその副産物 |
| [`../install/`](../install/) | OS インストール本体。 PS1 設定は `~/.kshrc` 経由なので OS 側とは独立 |

## ファイル構成

```
battery/
├── README.md
├── notes/
│   └── observations.md       実機観察ログ・キャリブレ記録
└── kshrc/
    ├── v3-factory-R045.kshrc          工場 R=45mΩ 採用版 (snapshot)
    ├── v3-cal-R120.kshrc              実測 R=120mΩ 採用版 (snapshot)
    ├── v3.1〜v3.5-*.kshrc             net 表示・色変更・bracket 化
    ├── v3.6〜v3.8-{actual,draft}-*    Nerd Font / OCV 補正復活
    └── v3.9-{actual,draft}-cat-only   netwatchd backend 化 (現行)
```

OCV table の数値部分はすべて `PLACE_OCV_FROM_FACTORY_DTB_21_VALUES_ASCENDING_MV`
placeholder にしてある。 自分の機体の値を入れて使うこと (取得手順は後述)。

## OCV 取得手順 (eMMC backup → factory dtb → ocv_table)

工場出荷状態の Android ベースのファームウェアには、 充放電カーブと電池の
内部抵抗をハードコードした device tree (dtb) が含まれている。 これは個別の
電池ロットに合わせて調整された値なので、 利用すれば voltage→SOC 変換が
かなり改善する。

### 1. eMMC backup (Android が生きているうちに)

OpenBSD を入れる前に、 **EKESETE などの DM250 eMMC backup tool** で本体
eMMC 全体を吸い出す (この tool 自体はこのリポでは扱わない)。 backup 後に
`recovery.img` や `boot.img` を抽出しておく。

### 2. dtb を抽出

`../logo/` の手順で `out/rk-kernel.dtb` を取り出す (`logo/` 自体は起動ロゴ
の抽出が主目的だが、 同じ dtb から OCV table も拾える)。

```bash
# 例: boot.img から kernel + dtb を分離
abootimg -x boot.img   # → zImage と kernel.dtb が出てくる
# rk-kernel.dtb をそのまま logo/out/ に置く
```

### 3. dtc で dts に decompile

```bash
dtc -I dtb -O dts -o rk-kernel.dts logo/out/rk-kernel.dtb
```

### 4. ocv_table を grep

```bash
grep -A 1 'ocv_table' rk-kernel.dts
```

`battery@0` node に以下のような形で出てくる:

```
battery@0 {
    compatible = "rockchip,rk818-battery";
    ocv_table = <0x... 0x... 0x... ...>;   /* 21 entries */
    design_capacity = <5800>;
    design_qmax = <6000>;
    bat_res = <0x2d>;       /* 45 mΩ */
    ...
};
```

21 個の `0x...` 値を **そのまま 10 進 (mV)** に変換し、 ascending 順 (3400→4200
mV) に並べる:

```bash
# 例: dts の 21 値を mV 列に直す
python3 -c '
vals = [0x..., 0x..., ...]   # dts から貼り付け
print(" ".join(str(v) for v in sorted(vals)))
'
```

### 5. kshrc に流し込む

`kshrc/v3-cal-R120.kshrc` の `PLACE_OCV_FROM_FACTORY_DTB_21_VALUES_ASCENDING_MV`
を、 取得した 21 値の半角 space 区切り文字列で置換する:

```awk
n = split("3400 PLACE_OCV_FROM_FACTORY_DTB_21_VALUES_ASCENDING_MV 4200", t, " ")
```

ascending 必須。 重複や 21 個未満だと SOC 計算がぶれる。

## R (内部抵抗) の測定法

OCV 補正の数式は `OCV = V ± I·R`。 ここで `R` は **電池の実効内部抵抗**で、
工場 dtb の `bat_res` 値は新品セル想定の瞬時オーミック (~45mΩ) なので経年
変化を捉えない。 自分で測ると DM250 個別の値が出る。

実機で 2 種類試して同じ値が出れば信頼できる:

### 方法 A: 負荷ステップ法 (ΔV / ΔI)

同一 SOC で CPU を `dd` ループ等で焼いて放電電流を上げ、 idle 状態との差分:

```bash
# on pomera, USB unplug 状態
# 1. idle 状態を 3 点平均
for i in 1 2 3; do
  _bat_debug
  sleep 5
done

# 2. dd 4 本で焼く
for i in 1 2 3 4; do dd if=/dev/zero of=/dev/null bs=1M & done
sleep 30
for i in 1 2 3; do
  _bat_debug
  sleep 5
done
killall dd

# 3. 解除後 settle
sleep 30
_bat_debug
```

`R = (V_idle - V_load) / (I_load - I_idle)`。

### 方法 B: 充電/放電 2 点法

同じ ~SOC で USB を抜き差しした両端の電圧/電流から:

```
R = (V_charge - V_discharge) / (I_charge + I_discharge)
```

USB 挿入直後 (10s 以内、 SOC が真値を保持しているうち) に測ること。

### 結果を kshrc に反映

両手法が一致したら `_BAT_R=0.12` などとセットする。 工場 dtb の 45mΩ では
plug/unplug 時に SOC が ±14pt 跳ねるが、 実測 ~120mΩ にすると ~±1pt まで
圧縮できる (本機での実測例)。

## PS1 `_bat` の進化 (時系列)

`~/.kshrc` の進化を `kshrc/` 配下に snapshot として残してある。 各 step で
**何を変えたか / 何を変えなかったか**を明確にするのが目的:

| バージョン | 内容 | 精度 | snapshot |
|---|---|---:|---|
| v1 | sysctl `percent0` をそのまま表示 | ±33% jump | (未保存) |
| v2 | OCV 補正 (R=0.10、 汎用 Li-ion curve) | ±10-15% | (未保存) |
| v3 | 工場 21 点 OCV table + R=0.045 | ±10-20% (plug jump +14pt 実測) | `kshrc/v3-factory-R045.kshrc` |
| v3-cal | v3 + 実測 R=0.12 (工場 OCV table 据置) | plug jump ~±1pt | `kshrc/v3-cal-R120.kshrc` |
| v3.1-v3.5 | net 表示・色変更・bracket 化 (精度据置) | 同上 | `kshrc/v3.1-*` 〜 `v3.5-*` |
| v3.6 | Nerd Font 化 (wifi/bt glyph、 FA4 BMP PUA に限定。 battery glyph は撤去 % のみ) | raw を素通し ±20% | `kshrc/v3.6-actual-nerdfont.kshrc` |
| v3.7 | WiFi=緑 / BT=青 配色 | 同上 | `kshrc/v3.7-actual-wifi-green-bt-blue.kshrc` |
| v3.8 | OCV 補正 awk を復活 (v3-cal の数式 + R=0.12 + 工場 21 点 table を Nerd Font 化版に乗せ) | plug jump ~±1pt | `kshrc/v3.8-actual-ocv-nerdfont.kshrc` |
| **v3.9** | **netwatchd backend に計算を移譲、 PS1 は `cat /tmp/.prompt-{bat,net}` のみ。 fork 7 → 2 で約 7-10 倍高速 (70-100ms → ~10ms / 評価)** | 同上 | **`kshrc/v3.9-actual-cat-only.kshrc`** |
| netwatchd 0.2 | plug-in/out 時の +10〜+12pt jump 対策。 state 変化検知 → 15 秒 hold + ramp ±1pt/poll。 `/tmp/.bat-meta` に prev_soc/prev_state/hold_until 永続化 | jump < ±3pt | (netwatchd 本体は別 subtree) |

> R 注記: v2 の R=0.10 は今回の実測 ~0.12Ω に近く、 v3 で工場値 0.045 に
> 下げたのは jump 抑制の観点では後退だった。 v3-cal は OCV table (工場値) は
> 残したまま R だけ実測値に戻した形。 較正の根拠は `notes/observations.md`
> の 2026-05-30 03:41 エントリ。

### actual / draft の使い分け

- `*-draft-*.kshrc` … 投入前の試作。 編集してから scp で実機に上げる。
- `*-actual-*.kshrc` … 実機投入が完了した snapshot。 ロールバック先。

実機への反映フロー:

```bash
# on mac
vim kshrc/v4-draft-foo.kshrc
scp kshrc/v4-draft-foo.kshrc \
    <your-pomera-user>@<dm250-lan-ip>:~/.kshrc
ssh <your-pomera-user>@<dm250-lan-ip> '. ~/.kshrc; _bat_debug'
# 動作 OK ら snapshot 確定
cp kshrc/v4-draft-foo.kshrc kshrc/v4-actual-foo.kshrc
```

## 観察ログ

`notes/observations.md` に随時 append。 形式例:

```markdown
## 2026-05-30 11:23  (v3 デプロイ後、 残量帯 ~75-95%)
- 放電中 (0.5A) : -79%
- 充電中 (0.85A): +95%
- プラグ抜き 10s : -75%
- jump 幅 = 20% (95→75)
- 環境: 室温 ~25°C、 sshd + Wi-Fi のみ稼働
```

## ライセンス姿勢

`kshrc/v3-*` 以降は **工場 dtb 由来の数値 (R=0.045、 21 点 OCV table)** を含む
形が前提だが、 公開版ではすべて `PLACE_OCV_FROM_FACTORY_DTB_21_VALUES_ASCENDING_MV`
placeholder に置換してある。 自機の dtb から取得した値を入れて使うこと。

新規ファイル (`kshrc/*.kshrc`) は `SPDX-License-Identifier: MIT` で配布する。
