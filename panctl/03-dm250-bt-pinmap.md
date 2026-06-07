# 03 - DM250 Bluetooth pinmap / 結線情報

[DESIGN.md](DESIGN.md) Phase B Step 1〜2 (UART インスタンス特定 + GPIO bring-up) に使う一次情報。jcs/linux-dm250 dts と jcs/openbsd-src rk3128 ブランチから 2026-05-26 に収集。

> **重要発見 (2026-05-26 ソース読解で確定)**: jcs/openbsd-src の rk3128 ブランチに **`sys/dev/fdt/bcmbt_fdt.c`** という BT bring-up ドライバが存在し、 boot 時に **GPIO bring-up + UART 初期化 + HCI Reset + patchram (`/etc/firmware/BCM4343A1.hcd`) 投入 + post-reset + BD_ADDR 読み出し** までを完了させる。 cdev は持たず (`DV_DULL`)、 親 `com(4)` の `/dev/cua00` を userland に残す。 結果 panctl 側 Phase B は「`/dev/cua00` を 115200 8N1 + CRTSCTS で open するだけ」に縮退（patchram 不要、 baud 切替不要）。 詳細 [`STATUS.md`](STATUS.md) §「bring-up 分岐確定」。

## 1. SoC / 無線モジュール

| 項目 | 値 |
|---|---|
| SoC | Rockchip **RK3128** (ARMv7, Cortex-A7 ×4) |
| BT chip | Broadcom **BCM43438** (compatible 文字列より。AP6212A = BCM43430A1 系と HCI 互換) |
| BT firmware | 配布元 **`BCM43430A1.hcd`** (RPi-Distro/bluez-firmware master) → kernel 期待 **`BCM4343A1.hcd`** にリネーム配置 (詳細: [`etc/firmware.README.md`](etc/firmware.README.md)) |

`compatible = "brcm,bcm43438-bt"` は Linux 側のバインディング名。 実物理チップは AP6212A 内蔵の BCM43430A1 リビジョンで、 HCI patchram バイナリは RPi 系では `BCM43430A1.hcd` で配布される。 OpenBSD カーネル (`bcmbt_fdt.c`) は `loadfirmware(9)` で `BCM4343A1.hcd` という名前を hardcode しているので **取得後にリネーム必須**（バイナリは同一）。

## 2. UART 配線

- **インスタンス: `uart0`** (RK3128 の UART0)
- 物理アドレス: `0x20060000`
- IRQ: GIC_SPI **20**
- compatible: `rockchip,rk3128-uart` / `snps,dw-apb-uart`
- ベースクロック: `SCLK_UART0` (24 MHz)
- 初期 baud: **115200 8N1** (Broadcom 標準。patchram 後に高速化)
- ハードフロー制御: **RTS/CTS 配線あり**。ただし `uart-rts-gpios` で GPIO override 可能 (HCI Download Minidriver 中に RTS を一時的に GPIO 制御する必要があるため)

**コンソール (`uart1`) との混同注意**: pomera のシリアルコンソールは `&uart1` (`0x20064000`, ttyS1) なので、`uart0` を BT 用に占有して問題ない。

OpenBSD 側のデバイス名は **`/dev/cua00`**（uart0 が `com0` に attach、 cua00 は通常のシリアル callout デバイス）。 `bcmbt_fdt.c` は `com(4)` を parent とした子デバイスとして attach するだけで、 独自の cdev は作らない (`DV_DULL`)。 boot 時の attach 後、 chip は HCI ready 状態のまま 115200 8N1 で座っているので、 userland は普通の TTY API でアクセスできる。

## 3. GPIO ピン配置

| 機能 | バンク・ピン | 極性 | 用途 |
|---|---|---|---|
| **BT_REG_ON** (`shutdown-gpios`) | **GPIO3_C5** | Active High | BT chip 電源 ON/OFF。High で起動 |
| **BT_WAKE** (`device-wakeup-gpios`) | **GPIO3_D2** | Active High | Host → BT に対する wake 信号 |
| **HOST_WAKE** (`host-wakeup-gpios`) | **GPIO3_C6** | **Active Low** | BT → Host への wake 信号 (interrupt 受け) |
| **BT_UART_RTS** (`uart-rts-gpios`) | **GPIO0_C1** | Active High | UART0 RTS を GPIO function に切替えるオーバーライド (patchram 中の HCI Reset 投入時に必要) |

ピンコントロール定義 (Linux dts より):

```dts
&pinctrl {
    bt {
        bt_reset:     bt-reset     { rockchip,pins = <3 RK_PC5 RK_FUNC_GPIO &pcfg_pull_none>; };
        bt_wake:      bt-wake      { rockchip,pins = <3 RK_PD2 RK_FUNC_GPIO &pcfg_pull_none>; };
        bt_host_wake: bt-host-wake { rockchip,pins = <3 RK_PC6 RK_FUNC_GPIO &pcfg_pull_none>; };
        bt_rts_gpio:  bt-rts-gpio  { rockchip,pins = <0 RK_PC1 RK_FUNC_GPIO &pcfg_pull_none>; };
        bt_rts_uart:  bt-rts-uart  { rockchip,pins = <0 RK_PC1 2 &pcfg_pull_none>; };
    };
};
```

`bt_rts_gpio` / `bt_rts_uart` の 2 state がある点に注意 — patchram 中は GPIO、通常運用は UART function に切替える。

## 4. UART0 の BT ノード全体 (一次情報)

`jcs/linux-dm250` `arch/arm/boot/dts/rockchip/pomera-dm250-wifi.dtsi` より:

```dts
/* for bluetooth */
&uart0 {
    status = "okay";
    bluetooth {
        compatible          = "brcm,bcm43438-bt";
        shutdown-gpios      = <&gpio3 RK_PC5 GPIO_ACTIVE_HIGH>;
        device-wakeup-gpios = <&gpio3 RK_PD2 GPIO_ACTIVE_HIGH>;
        host-wakeup-gpios   = <&gpio3 RK_PC6 GPIO_ACTIVE_LOW>;
        uart-rts-gpios      = <&gpio0 RK_PC1 GPIO_ACTIVE_HIGH>;
        pinctrl-names = "default", "rts-gpio", "rts-uart";
        pinctrl-0 = <&bt_reset &bt_wake &bt_host_wake>;
        pinctrl-1 = <&bt_rts_gpio>;
        pinctrl-2 = <&bt_rts_uart>;
    };
};
```

## 5. jcs/openbsd-src 側の状況

- `sys/dev/fdt/bcmbt_fdt.c` という drv が rk3128 ブランチに既に存在 (`OF_is_compatible(..., "brcm,bcm43438-bt")` で match)
- これが GPIO bring-up + 初期 patchram までやる構成なら、**panctl 側は GPIO を触らず `/dev/cua00` (uart0 の char device) を libttsh4 backend で開くだけで済む**
- ~~ただし `bcmbt_fdt.c` が:~~ （原仮説、 下記「ソース読解で確定」で解消）
  - ~~**(A)** 単に GPIO bring-up と clock 制御だけして UART は通常の `com(4)` として attach するパターン~~
  - ~~**(B)** HCI commands も内部で処理して `ugen` 風の擬似 char dev を出すパターン~~
  - ~~**(C)** カーネル内で patchram までやり、ホストには 921600 / 3 Mbps の plain UART を出すパターン~~

**2026-05-26 ソース読解で確定**: 3 択どれにも厳密には当てはまらない、 「(A) 拡張 = kernel が GPIO + UART init + HCI Reset + Download Minidriver + patchram + post-reset + BD_ADDR まで全部やる、 ただし transport は普通の `com(4) /dev/cua00`、 baud は 115200 固定のまま」分岐。 userland への含意は (C) と (A) の中間で、 patchram と baud 切替が不要な点は (B/C) と同じ。 詳細 [`STATUS.md`](STATUS.md) §「bring-up 分岐確定」。

確定後の対応:

- BTstack `port/posix-h4` を `/dev/cua00` 経由で開くだけ。 GPIO / patchram / baud 切替は触らない
- **BTstack の `chipset/bcm` を呼ばない**（chipset は `chipset/none` で OK。 userland 側 patchram を再投入するとカーネルがすでに済ませた後で衝突する）
- Phase B のスコープは大幅に縮み、 panctl 側の `bt_reg_on` / `transport_h4.c` の bring-up 部分は **実装不要**

## 6. branch / commit (再現性のため)

| 項目 | 値 | 取得日 |
|---|---|---|
| `jcs/linux-dm250` HEAD (master) | `219a94ff9cb7fc2ec57a0e6a1156f5ba5ed62f40` | 2026-05-26 |
| `jcs/openbsd-src` HEAD (rk3128) | `58e7c7d38b8081f3949032e461bace5f138148c0` | 2026-05-26 |

`pomera-dm250-wifi.dtsi` / `pomera-dm250.dts` / `rk3128.dtsi` / `bcmbt_fdt.c` はいずれも上記 commit からの抜粋。再取得手順:

```sh
# on mac
curl -L https://raw.githubusercontent.com/jcs/linux-dm250/master/arch/arm/boot/dts/rockchip/pomera-dm250-wifi.dtsi
curl -L https://raw.githubusercontent.com/jcs/openbsd-src/rk3128/sys/dev/fdt/bcmbt_fdt.c
```

## 7. Phase B での使い道（分岐確定後）

panctl `transport_h4.c` でやることは以下に縮退:

1. `/dev/cua00` を `O_RDWR | O_NOCTTY` で open
2. `termios` で **115200 8N1 + CRTSCTS** に設定（baud 切替なし）
3. BTstack の `btstack_uart_block_posix.c` をそのまま使い、 chipset は `chipset/none`
4. HCI run loop に入る

実装不要になったもの:

- ~~`gpio(4)` 経由 `BT_REG_ON` 制御~~ → kernel `bcmbt_fdt.c` がやる
- ~~`btstack_chipset_bcm` で `BCM43430A1.hcd` upload~~ → kernel がやる
- ~~`HCI Vendor Specific Update Baud Rate` 921600 切替~~ → 115200 のまま
- ~~`tools/bt_reg_on.c`~~ → 不要（[`DESIGN.md`](DESIGN.md) §4 のディレクトリ構成からは Phase A の他ハード検証用として残置可だが、 DM250 では使わない）

HOST_WAKE (GPIO3_C6 active-low) の interrupt 受けも引き続き **Phase B では未実装で OK**。 sleep 制御を切る方針なので無くて動く。 低消費電力対応は Phase C 以降。

## 8. その他メモ

- BT_HOST_WAKE が **Active Low** なのは Broadcom 慣習。Linux side は割り込みで使うが、panctl では当面ポーリングか無視で OK
- BT chip の OTP に MAC アドレスが焼かれているはず。HCI_Read_BD_ADDR (0x1009) で取得して Android アプリ側の allow-list に登録する流れ
- 万一 Linux dts と OpenBSD `bcmbt_fdt.c` で結線が食い違う場合、`bcmbt_fdt.c` の `OF_getpropbool` / `OF_getpropintarray` 呼び出しを優先 (実機で動く方が正)
