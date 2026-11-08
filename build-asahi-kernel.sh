#!/bin/bash
# Build (or stage) the Asahi Linux 16 KB kernel, DeviceTrees (t8103-j313.dtb), m1n1, and U-Boot
# for bare-metal Googlebook OS on Apple Silicon (MacBookAir10,1 / M1 / t8103).
#
# Output directory: $WORK/asahi-boot/
#   - Image                (ARM64 16K-page kernel with Binder, Ashmem, drm-asahi, apple-dcp, macaudio)
#   - dtb/t8103-j313.dtb   (Apple M1 MacBook Air 13" DeviceTree)
#   - m1n1.bin             (Apple Silicon stage 1/2 bootloader)
#   - u-boot-nodtb.bin     (Das U-Boot Apple Silicon payload)
set -e
. "$(dirname "$0")/lib.sh"

ASAHI_BOOT="$WORK/asahi-boot"
mkdir -p "$ASAHI_BOOT/dtb/apple"

ASAHI_KERNEL_REPO="${ASAHI_KERNEL_REPO:-https://github.com/AsahiLinux/linux.git}"
ASAHI_KERNEL_BRANCH="${ASAHI_KERNEL_BRANCH:-asahi-6.12.y}"
M1N1_REPO="${M1N1_REPO:-https://github.com/AsahiLinux/m1n1.git}"
UBOOT_REPO="${UBOOT_REPO:-https://github.com/AsahiLinux/u-boot.git}"

if [ "${BUILD_ASAHI_FROM_SOURCE:-0}" = "1" ]; then
  say "Creating case-sensitive APFS workspace for Asahi kernel & bootloader build"
  KVOL="$WORK/asahi-kbuild.sparseimage"
  KMOUNT="$WORK/asahi-kmount"
  if [ ! -e "$KVOL" ]; then
    hdiutil create -size 25g -type SPARSE -fs "Case-sensitive APFS" -volname AsahiKBuild "$KVOL" >/dev/null
  fi
  mkdir -p "$KMOUNT"
  hdiutil attach "$KVOL" -mountpoint "$KMOUNT" -nobrowse >/dev/null || true

  [ -d "$KMOUNT/m1n1" ] || git clone --depth 1 --recursive "$M1N1_REPO" "$KMOUNT/m1n1"
  [ -d "$KMOUNT/u-boot" ] || git clone --depth 1 "$UBOOT_REPO" "$KMOUNT/u-boot"
  [ -d "$KMOUNT/linux" ] || git clone --depth 1 -b "$ASAHI_KERNEL_BRANCH" "$ASAHI_KERNEL_REPO" "$KMOUNT/linux"

  say "Merging configs/asahi-android-16k.config into Asahi kernel config"
  cp "$ROOT/configs/asahi-android-16k.config" "$KMOUNT/linux/arch/arm64/configs/asahi_android_16k.config"
  (cd "$KMOUNT/linux" && make ARCH=arm64 LLVM=1 defconfig asahi_android_16k.config && make -j "$JOBS" ARCH=arm64 LLVM=1 Image dtbs)
  cp -f "$KMOUNT/linux/arch/arm64/boot/Image" "$ASAHI_BOOT/Image"
  cp -f "$KMOUNT/linux/arch/arm64/boot/dts/apple/"*.dtb "$ASAHI_BOOT/dtb/apple/"
else
  say "Staging kernel and config fragment (set BUILD_ASAHI_FROM_SOURCE=1 to compile full AsahiLinux/linux tree)"
  [ -f "$WORK/asahi-image/kernel.Image" ] && cp -f "$WORK/asahi-image/kernel.Image" "$ASAHI_BOOT/Image"
  cp -f "$ROOT/configs/asahi-android-16k.config" "$ASAHI_BOOT/asahi-android-16k.config"
fi

say "Asahi boot artifacts staged in $ASAHI_BOOT"
ls -la "$ASAHI_BOOT"
