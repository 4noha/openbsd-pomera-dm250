#!/bin/ksh
# prep-sd.sh — Run inside OpenBSD install79 installer shell to prep the DM250
# install SD card. Pulls files from a Mac HTTP server (default 10.0.2.2:8000
# under qemu user-mode networking).
#
# Boot install79.img in qemu with the SD as a second virtio drive, press S
# at the welcome prompt, then:
#   ftp -V -o /tmp/prep-sd.sh http://10.0.2.2:8000/prep-sd.sh
#   sh /tmp/prep-sd.sh
#
# Boot disk (install image) becomes sd0; the SD card becomes sd1.

set -eu

HOST="${HOST:-10.0.2.2:8000}"
SD="${SD:-sd1}"
SNAP_VER="${SNAP_VER:-79}"        # base79.tgz / xshare79.tgz etc.
BWFM_VER="${BWFM_VER:-20200316.1.3p5}"

say()  { echo; echo "=== $* ==="; }
die()  { echo "ERR: $*" >&2; exit 1; }

say "preflight"
ifconfig vio0 2>/dev/null | grep -q 'inet 10\.0\.2\.' \
    || die "vio0 has no qemu user-mode IP yet"
dmesg | grep -qE "^${SD}.*MB" \
    || die "${SD} not found in dmesg"
echo "target  : ${SD}"

# The installer ramdisk only pre-creates device nodes for sd0. Make nodes
# for the target SD so fdisk/disklabel/newfs can open it.
( cd /dev && sh ./MAKEDEV "$SD" ) || die "MAKEDEV ${SD} failed"
ls -l "/dev/${SD}c" "/dev/r${SD}c" || die "${SD} device nodes missing after MAKEDEV"

if [ "${YES:-}" != "1" ]; then
    read 'CONFIRM?Wipe ${SD} and write OpenBSD install layout? [yes/NO] '
    [ "$CONFIRM" = "yes" ] || die "aborted"
fi

say "step 1: GPT + filesystems"
# fdisk -g creates the GPT with the 100MB EFI partition (-b 204800) AND
# an OpenBSD partition covering the rest. Modern disklabel auto-exposes
# the OpenBSD MBR/GPT slot as partition 'a' (4.2BSD) and the MSDOS slot
# as partition 'i', so no explicit disklabel editing is needed.
fdisk -ygb 204800 "$SD"
newfs "/dev/r${SD}a"
newfs_msdos "/dev/r${SD}i"

say "step 2: EFI/MSDOS partition"
mkdir -p /mnt
mount "/dev/${SD}i" /mnt
mkdir -p /mnt/efi/boot
cd /mnt/efi/boot
ftp -V "http://${HOST}/armv7-snapshot/BOOTARM.EFI"
# Note: signify(1) verification skipped here. The files are pulled from
# loopback HTTP off the Mac-side filesystem, where the snapshot was
# already verified end-to-end with `shasum -c SHA256` at download time.
cd /mnt
ftp -V -o uboot.img    "http://${HOST}/dm250/uboot.img"
ftp -V -o _sdboot.sh   "http://${HOST}/dm250/_sdboot.sh"
ftp -V -o logo.bmp     "http://${HOST}/dm250/logo.bmp"
cd /
umount /mnt

say "step 3: OpenBSD FFS partition (sets + bsd + bsd.rd + bwfm firmware)"
mount "/dev/${SD}a" /mnt
cd /mnt
# Keep the snapshot's SHA256.sig + INSTALL.armv7 with the sets so the
# pomera-side installer dialog can verify and discover them.
ftp -V "http://${HOST}/armv7-snapshot/SHA256.sig"
ftp -V "http://${HOST}/armv7-snapshot/INSTALL.armv7"
for s in base${SNAP_VER} comp${SNAP_VER} game${SNAP_VER} man${SNAP_VER} \
         xbase${SNAP_VER} xfont${SNAP_VER} xserv${SNAP_VER} xshare${SNAP_VER}; do
    ftp -V "http://${HOST}/armv7-snapshot/${s}.tgz"
done
# Signify verification skipped (see step 2 comment).
ftp -V "http://${HOST}/firmware/bwfm-firmware-${BWFM_VER}.tgz"
ftp -V -o bsd     "http://${HOST}/dm250/bsd"
ftp -V -o bsd.rd  "http://${HOST}/dm250/bsd.rd"
cd /
umount /mnt

sync

say "done"
ls -l "/dev/r${SD}"? 2>/dev/null || true
echo
echo "SD-PREP-COMPLETE"
echo "SD ready. Halt the VM (halt -p) and eject the SD on the Mac."
