#!/bin/bash
# Package the bare-metal Googlebook OS image + initramfs (with pre_init_asahi dm-linear mapper)
# into a standard Asahi Linux distro installer layout ($WORK/asahi-installer/).
#
# Output:
#   - $WORK/asahi-installer/installer_data.json     (asahi-installer OS catalog manifest)
#   - $WORK/asahi-installer/esp/                    (EFI System Partition tree for U-Boot/GRUB/m1n1)
#   - $WORK/asahi-installer/gbos-asahi-esp.zip      (ESP archive for asahi-installer)
#   - $WORK/asahi-installer/install-asahi-baremetal.sh (Interactive local installer launcher)
set -e
. "$(dirname "$0")/lib.sh"

ASAHI_IMG_DIR="${GBOS_ASAHI_IMAGE_DIR:-$WORK/asahi-image}"
[ -f "$ASAHI_IMG_DIR/googlebook.raw" ] || die "Run: GBOS_IMAGE_DIR=\$WORK/asahi-image GBOS_IMAGE_FLAGS=--asahi-baremetal ./build-image.sh first"

OUT="$WORK/asahi-installer"
ESP="$OUT/esp"
rm -rf "$OUT"
mkdir -p "$ESP/m1n1" "$ESP/efi/boot" "$ESP/gbos/dtb/apple" "$ESP/vendorfw"

say "Populating EFI System Partition (ESP) boot tree"
cp -f "$ASAHI_IMG_DIR/initrd.img" "$ESP/gbos/initrd.img"
if [ -f "$WORK/asahi-boot/Image" ]; then
  cp -f "$WORK/asahi-boot/Image" "$ESP/gbos/Image"
else
  cp -f "$ASAHI_IMG_DIR/kernel.Image" "$ESP/gbos/Image"
fi
if compgen -G "$WORK/asahi-boot/dtb/apple/*.dtb" >/dev/null 2>&1; then
  cp -f "$WORK/asahi-boot/dtb/apple/"*.dtb "$ESP/gbos/dtb/apple/"
fi

cat > "$ESP/gbos/boot.cmd" <<'BOOTCMD'
# U-Boot boot script for Googlebook OS Bare-Metal on Apple Silicon (M1 / t8103 / j313)
echo "Booting Googlebook OS (Android 17 Bare-Metal Workstation) on Apple Silicon..."
setenv bootargs "console=ttySAC0,1500000 console=tty0 earlycon root=/dev/ram0 rw clk_ignore_unused pd_ignore_unused androidboot.hardware=android-desktop androidboot.selinux=enforcing androidboot.product.vendor.sku=vmbt firmware_class.path=/vendor/firmware,/vendor_fw/firmware snd-aloop.index=0"
load ${devtype} ${devnum}:${distro_bootpart} ${kernel_addr_r} /gbos/Image
load ${devtype} ${devnum}:${distro_bootpart} ${ramdisk_addr_r} /gbos/initrd.img
setenv ramdisk_size ${filesize}
if test -e ${devtype} ${devnum}:${distro_bootpart} /gbos/dtb/${fdtfile}; then
    load ${devtype} ${devnum}:${distro_bootpart} ${fdt_addr_r} /gbos/dtb/${fdtfile}
    booti ${kernel_addr_r} ${ramdisk_addr_r}:${ramdisk_size} ${fdt_addr_r}
else
    booti ${kernel_addr_r} ${ramdisk_addr_r}:${ramdisk_size} ${fdtcontroladdr}
fi
BOOTCMD

say "Creating ESP archive and computing installer_data.json metadata"
(cd "$ESP" && zip -q -r "$OUT/gbos-asahi-esp.zip" .)

RAW_BYTES="$(stat -f%z "$ASAHI_IMG_DIR/googlebook.raw")"
ESP_BYTES="$(stat -f%z "$OUT/gbos-asahi-esp.zip")"

cat > "$OUT/installer_data.json" <<JSON
{
  "os_list": [
    {
      "name": "Googlebook OS 17 (Apple Silicon Bare-Metal Workstation)",
      "default_os": "Googlebook OS",
      "boot_object": "m1n1.bin",
      "next_object": "u-boot-nodtb.bin",
      "package": "gbos-asahi-esp.zip",
      "supported_models": ["j313", "j274", "j293", "j314", "j316", "j413", "j493"],
      "partitions": [
        {
          "name": "EFI",
          "type": "EFI",
          "size": "500MB",
          "format": "fat",
          "volume_id": "0x2a47424f",
          "copy_firmware": true,
          "copy_installer_data": true,
          "source": "/"
        },
        {
          "name": "Googlebook OS",
          "type": "Linux",
          "size": "${RAW_BYTES}B",
          "expand": true,
          "image": "googlebook.raw"
        }
      ]
    }
  ]
}
JSON

ln -sf "$ASAHI_IMG_DIR/googlebook.raw" "$OUT/googlebook.raw"

cat > "$OUT/install-asahi-baremetal.sh" <<'INSTALLER'
#!/bin/bash
# Interactive Asahi Linux Bare-Metal Installer Launcher for Googlebook OS
# Serves the local Googlebook OS installer_data.json and partition payloads to asahi-installer
# so you can choose your internal NVMe partition size just like any standard Asahi Linux distro.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
echo "======================================================================"
echo " Googlebook OS (Android 17) - Apple Silicon Bare-Metal Distro Bundle"
echo "======================================================================"
echo "Bundle directory : $DIR"
echo "Manifest         : $DIR/installer_data.json"
echo "ESP archive      : $DIR/gbos-asahi-esp.zip"
echo "OS container     : $DIR/googlebook.raw"
echo ""
echo "To install onto your M1 Mac's internal NVMe disk with interactive"
echo "partition resizing via the standard Asahi Linux installer:"
echo "  1. Start a local HTTP server in this directory (or run with --serve):"
echo "       ./install-asahi-baremetal.sh --serve"
echo "  2. Launch the official Asahi installer pointed at the local catalog:"
echo "       export ASAHI_INSTALLER_BASE_URL=http://127.0.0.1:8000"
echo "       curl -sL https://alx.sh | sh"
echo ""
if [ "${1:-}" = "--serve" ]; then
    echo "Serving $DIR on http://127.0.0.1:8000 (Ctrl-C to stop)..."
    exec python3 -m http.server 8000 --directory "$DIR"
fi
INSTALLER
chmod +x "$OUT/install-asahi-baremetal.sh"

say "Asahi bare-metal installer bundle ready: $OUT"
ls -lh "$OUT"
