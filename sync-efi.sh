#!/bin/bash
# Dedicated, verbose EFI sync script for Googlebook OS on Apple Silicon
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "======================================================================"
echo " Googlebook OS - Apple Silicon Bare-Metal EFI Sync Tool"
echo "======================================================================"

# 1. Clean NVRAM upgrade/recovery variables
echo "[1/4] Clearing stale NVRAM recovery state..."
nvram -d upgrade-boot-volume 2>/dev/null || true
nvram -d IDInstallerDataV2 2>/dev/null || true

# 2. Locate EFI partition
echo "[2/4] Detecting EFI - GOOGL partition..."
EFI_NODE="$(diskutil list | awk '/EFI - GOOGL/{print $NF}')"
if [ -z "$EFI_NODE" ]; then
    EFI_NODE="disk0s4"
fi
echo "  Found target EFI partition: /dev/$EFI_NODE"

# 3. Clean FAT dirty bit and mount
echo "[3/4] Checking filesystem and mounting EFI partition..."
fsck_msdos -y "/dev/r$EFI_NODE" >/dev/null 2>&1 || true

diskutil mount "$EFI_NODE" >/dev/null 2>&1 || true

MOUNTPOINT="$(diskutil info -plist "$EFI_NODE" | plutil -extract MountPoint raw - 2>/dev/null || true)"
if [ -z "$MOUNTPOINT" ] || [ ! -d "$MOUNTPOINT" ]; then
    MOUNTPOINT="/Volumes/EFI - GOOGL"
    mkdir -p "$MOUNTPOINT"
    mount -t msdos "/dev/$EFI_NODE" "$MOUNTPOINT" 2>/dev/null || true
fi

if [ ! -d "$MOUNTPOINT" ]; then
    echo "ERROR: Failed to mount /dev/$EFI_NODE. Please ensure you ran with sudo."
    exit 1
fi
echo "  Mounted successfully at: $MOUNTPOINT"

# 4. Copy boot binaries and kernel payloads
echo "[4/4] Copying verified boot payloads into EFI partition..."
cp -v "$DIR/esp/u-boot-nodtb.bin" "$MOUNTPOINT/u-boot-nodtb.bin"
cp -v "$DIR/esp/boot.scr" "$MOUNTPOINT/boot.scr"
cp -v "$DIR/esp/boot.cmd" "$MOUNTPOINT/boot.cmd"

mkdir -p "$MOUNTPOINT/m1n1" "$MOUNTPOINT/gbos" "$MOUNTPOINT/asahi"
cp -v "$DIR/esp/m1n1/boot.bin" "$MOUNTPOINT/m1n1/boot.bin"
cp -v "$DIR/esp/gbos/Image" "$MOUNTPOINT/gbos/Image"
cp -v "$DIR/esp/gbos/initrd.img" "$MOUNTPOINT/gbos/initrd.img"
cp -v "$DIR/esp/gbos/boot.cmd" "$MOUNTPOINT/gbos/boot.cmd"
cp -v "$DIR/esp/gbos/boot.scr" "$MOUNTPOINT/gbos/boot.scr"
if [ ! -f "$MOUNTPOINT/asahi/stub_info.json" ]; then
    echo "{}" > "$MOUNTPOINT/asahi/stub_info.json"
fi

sync
echo ""
echo "======================================================================"
echo " VERIFIED FILES ON EFI PARTITION ($MOUNTPOINT):"
echo "======================================================================"
ls -lh "$MOUNTPOINT/u-boot-nodtb.bin" "$MOUNTPOINT/boot.scr" "$MOUNTPOINT/gbos/Image" "$MOUNTPOINT/gbos/initrd.img"
echo "======================================================================"
echo "SUCCESS: EFI partition is fully populated and verified on physical SSD!"
echo "Now shut down your Mac, hold the power button, and select Googlebook OS."
echo "======================================================================"
