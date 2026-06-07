<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 4noha -->

# `bcmbt_fdt.c` post-firmware delay extension — design note

This is the design note behind the `bcmbt-delay-2s` kernel patch (the only Bluetooth patch in `kernel-patches/`). The patch extends the post-firmware HCI Reset delay in `bcmbt_fdt.c` from 250 ms to 2 s so that the BCM43430A1 used on the DM250 can complete bring-up. It was developed on top of the jcs/openbsd-src rk3128 branch.

The procedural steps below were originally the in-tree native-build plan. They are kept here as a historical reference / fallback. The recommended path today is the cross-build flow in [`cross-build-kernel.md`](cross-build-kernel.md); see the section on routes at the end of this document.

> [!NOTE]
> Per-license-inheritance: the patch itself modifies OpenBSD source under `sys/` and therefore inherits ISC. See `LICENSE-ISC` and the header comment in the patch file. This design note is MIT, like the rest of the project documentation.

## Background

- Detailed history of the bring-up split and the firmware-vs-Wi-Fi interaction lives in the `panctl/` notes (bring-up routes, firmware load / Wi-Fi collision narrative).
- Working state at design time (2026-05-29): BT firmware was kept disabled (`/etc/firmware/BCM4343A1.hcd.bak`) so that the box ran Wi-Fi reliably. `bcmbt0` attached but never received the patchram (it sat in low-power idle).
- With this patch applied: patchram completes → on-chip hardware coex lets Wi-Fi and BT coexist, no reboot needed, both radios available simultaneously. This is the precondition that `panctl/` needs.

## What the patch changes

File: `sys/dev/fdt/bcmbt_fdt.c`, inside `bcmbt_load_firmware()`:

```diff
 	/* wait for chip reset after firmware download */
-	delay(250000);
+	delay(2000000);  /* 2s for BCM43430A1 / DM250 (was 250ms; insufficient) */
```

If the simple delay change is not enough, an optional retry can be added around the post-firmware HCI Reset itself:

```diff
 	/* send post-firmware HCI Reset */
-	if (bcmbt_send_cmd(sc, hci_reset, sizeof(hci_reset)) != 0 ||
-	    bcmbt_recv_evt(sc, resp, sizeof(resp), &rlen) != 0) {
-		printf("%s: post-firmware HCI reset failed\n",
-		    sc->sc_dev.dv_xname);
-		return;
-	}
+	for (int retry = 0; retry < 5; retry++) {
+		if (bcmbt_send_cmd(sc, hci_reset, sizeof(hci_reset)) == 0 &&
+		    bcmbt_recv_evt(sc, resp, sizeof(resp), &rlen) == 0)
+			break;
+		delay(500000);
+		if (retry == 4) {
+			printf("%s: post-firmware HCI reset failed after 5 tries\n",
+			    sc->sc_dev.dv_xname);
+			return;
+		}
+	}
```

Start with the delay-only variant — it is the conservative change. Add the retry only if the delay alone is not enough.

## Procedure (legacy native-build path)

This is what was originally documented for building on the device. **It is slow (~30–90 minutes) and stresses the box**; prefer the cross-build path. Steps are kept for reference and for the case where the build VM is unavailable.

### Phase 2A — Get the source (on the device)

```sh
# on pomera (installed)
# At least 1 GB free on /
df -h /

# Install git
doas pkg_add git

# Prepare /usr/src
doas mkdir -p /usr/src
doas chown <your-pomera-user>:wheel /usr/src

# Shallow-clone the jcs rk3128 branch to save time and disk space
cd /usr/src
git clone --depth 1 -b rk3128 https://github.com/jcs/openbsd-src.git .

# Sanity check
ls -la sys/dev/fdt/bcmbt_fdt.c
```

### Phase 2B — Apply the patch

```sh
# on pomera (installed)
cd /usr/src
# delay(250000); -> delay(2000000);
doas sed -i.orig 's/delay(250000);.*wait for chip reset.*/delay(2000000);\t\/* DM250 patch: was 250ms *\//' \
    sys/dev/fdt/bcmbt_fdt.c
# Verify
grep -A1 'wait for chip reset' sys/dev/fdt/bcmbt_fdt.c
```

### Phase 2C — Build the kernel

```sh
# on pomera (installed)
cd /usr/src/sys/arch/armv7/conf
doas config GENERIC
cd ../compile/GENERIC
doas make obj
doas make
# 30-90 minutes on a single-core 1.2 GHz armv7
```

You can watch progress with `top` from a second ssh session.

### Phase 2D — Deploy

```sh
# on pomera (installed)
# Keep the stock (jcs-original) kernel as a fallback
doas cp /bsd /bsd.jcs-original

# Install the new one
doas cp /usr/src/sys/arch/armv7/compile/GENERIC/obj/bsd /bsd

# Restore the BT firmware to its active filename
doas mv /etc/firmware/BCM4343A1.hcd.bak /etc/firmware/BCM4343A1.hcd

# Reboot
doas reboot
```

### Phase 2E — Verify

After reboot, ssh in and:

```sh
# on pomera (installed)
# Did bcmbt attach get all the way to a BD_ADDR?
dmesg | grep bcmbt
# Expect: "bcmbt0: address XX:XX:XX:XX:XX:XX"

# Is Wi-Fi still healthy in parallel?
ifconfig bwfm0 | grep -E 'status:|inet 192'
# Expect: "status: active" and an inet on your LAN, e.g. "inet <dm250-lan-ip>"

# Latency / coex sanity
ping -c 5 <your-lan-gw>   # AP/gateway round-trip should not be much worse than before
```

## Failure recovery

If the new kernel misbehaves, fall back to the previous one.

### A. Boots, but BT is still broken

```sh
# on pomera (installed)
doas mv /etc/firmware/BCM4343A1.hcd /etc/firmware/BCM4343A1.hcd.bak
doas reboot
# Back to the "BT firmware disabled" workaround
```

### B. Kernel panic / will not boot

At the U-Boot `boot>` prompt, pick the saved kernel:

```
boot> b /bsd.jcs-original
```

Once booted:

```sh
# on pomera (installed)
doas mv /bsd /bsd.broken
doas mv /bsd.jcs-original /bsd
doas reboot
```

### C. Will not boot even with the fallback (worst case)

Boot from the SD card's `bsd.rd`, mount the eMMC root, and overwrite `/mnt/bsd`. See the recovery procedure in the install guide.

## Capturing the patch back into the repo

After a successful build, produce a patch file and commit it under `kernel-patches/`:

```sh
# on pomera (installed)
cd /usr/src
diff -u sys/dev/fdt/bcmbt_fdt.c.orig sys/dev/fdt/bcmbt_fdt.c > /tmp/bcmbt-delay-extend.patch
```

Pull that into `kernel-patches/bcmbt-delay-2s.patch` on the Mac side. The same patch is a candidate for an upstream PR to jcs.

## How this fits with the rest of the build flow

- **Preferred (today)**: cross-build the kernel under the arm64 VM described in [`cross-build-kernel.md`](cross-build-kernel.md). `bcmbt-delay-2s.patch` is one of the patches applied there; the delay/retry design in this document is the rationale for it.
- **Native (legacy)**: the Phases 2A–2E above. Slow, stresses the box. Useful only when the cross-build VM is unavailable.
- **Workaround mode**: keep `/etc/firmware/BCM4343A1.hcd.bak` and run Wi-Fi only — no kernel rebuild required, but no BT either.

## Progress

- [ ] Phase 2A — fetch source
- [ ] Phase 2B — apply patch
- [ ] Phase 2C — build kernel
- [ ] Phase 2D — deploy
- [ ] Phase 2E — verify
- [ ] Commit patch under `kernel-patches/`
