#!/bin/bash
# Package the bare-metal Googlebook OS image + initramfs (with pre_init_asahi dm-linear mapper)
# into a standard Asahi Linux distro installer layout ($WORK/asahi-installer/).
#
# Output:
#   - $WORK/asahi-installer/installer_data.json        (asahi-installer OS catalog manifest)
#   - $WORK/asahi-installer/esp/                       (EFI System Partition tree for U-Boot/GRUB/m1n1)
#   - $WORK/asahi-installer/os/gbos-asahi-esp.zip      (ESP archive for asahi-installer)
#   - $WORK/asahi-installer/install-asahi-baremetal.sh (Interactive Asahi installer launcher)
set -e
. "$(dirname "$0")/lib.sh"

ASAHI_IMG_DIR="${GBOS_ASAHI_IMAGE_DIR:-$WORK/asahi-image}"
[ -f "$ASAHI_IMG_DIR/googlebook.raw" ] || die "Run: GBOS_IMAGE_DIR=\$WORK/asahi-image GBOS_IMAGE_FLAGS=--asahi-baremetal ./build-image.sh first"

OUT="$WORK/asahi-installer"
ESP="$OUT/esp"
rm -rf "$OUT"
mkdir -p "$ESP/m1n1" "$ESP/efi/boot" "$ESP/gbos/dtb/apple" "$OUT/os"

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

# Ensure standard m1n1 stage 2 + U-Boot binary exists in esp/m1n1/boot.bin and esp/u-boot-nodtb.bin
if [ ! -f "$ESP/m1n1/boot.bin" ]; then
  python3 -c '
import urllib.request, zipfile, io, os
url = "https://cdn.asahilinux.org/os/uefi-only-20261002-asahi-7.1.13-2.zip"
req = urllib.request.Request(url)
with urllib.request.urlopen(req) as resp:
    data = resp.read()
zf = zipfile.ZipFile(io.BytesIO(data))
with open("'$ESP'/m1n1/boot.bin", "wb") as f:
    f.write(zf.read("esp/m1n1/boot.bin"))
'
fi
cp -f "$ESP/m1n1/boot.bin" "$ESP/u-boot-nodtb.bin"

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
(cd "$OUT" && zip -q -r "$OUT/os/gbos-asahi-esp.zip" esp)

RAW_BYTES="$(stat -f%z "$ASAHI_IMG_DIR/googlebook.raw")"

cat > "$OUT/installer_data.json" <<JSON
{
  "os_list": [
    {
      "name": "Googlebook OS 17 (Apple Silicon Bare-Metal Workstation)",
      "default_os_name": "Googlebook OS",
      "boot_object": "m1n1.bin",
      "next_object": "m1n1/boot.bin",
      "package": "gbos-asahi-esp.zip",
      "supported_models": ["j313", "j274", "j293", "j314", "j316", "j413", "j493"],
      "supported_fw": ["13.5", "14.8.3"],
      "partitions": [
        {
          "name": "EFI",
          "type": "EFI",
          "size": "500MB",
          "format": "fat",
          "volume_id": "0x2a47424f",
          "copy_firmware": true,
          "copy_installer_data": true,
          "source": "esp"
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
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "======================================================================"
echo " Googlebook OS (Android 17) - Apple Silicon Bare-Metal Distro Bundle"
echo "======================================================================"
echo "Bundle directory : $DIR"
echo "Manifest         : $DIR/installer_data.json"
echo "ESP package      : $DIR/os/gbos-asahi-esp.zip"
echo "OS container     : $DIR/googlebook.raw"
echo ""

if [ "${1:-}" = "--install" ]; then
    rm -f "$DIR/os/gbos-asahi-full.zip.tmp" "$DIR/os/gbos-asahi-full.zip"
    TMP="/tmp/asahi-install"
    mkdir -p "$TMP"
    cd "$TMP"
    echo "[1/2] Preparing official Asahi Linux installer framework..."
    PKG_VER="$(curl --no-progress-meter -L https://cdn.asahilinux.org/installer/latest)"
    if [ ! -f "installer-$PKG_VER.tar.gz" ]; then
        curl --no-progress-meter -L -o "installer-$PKG_VER.tar.gz" "https://cdn.asahilinux.org/installer/installer-$PKG_VER.tar.gz"
    fi
    tar xf "installer-$PKG_VER.tar.gz"
    cp -f "$DIR/installer_data.json" "$TMP/installer_data.json"
    # Enable zero-copy local streaming from $REPO_BASE/googlebook.raw (so we never
    # waste 34 GB of disk space duplicating googlebook.raw into a temporary zip archive),
    # pass dirs_exist_ok=True when copying vendorfw, and allow option 'p' (Repair an
    # incomplete installation) to resume an interrupted stage-1 OS image extraction.
    OSINSTALL_PY="$TMP/osinstall.py"
    [ -f "$TMP/src/osinstall.py" ] && OSINSTALL_PY="$TMP/src/osinstall.py"
    MAIN_PY="$TMP/main.py"
    [ -f "$TMP/src/main.py" ] && MAIN_PY="$TMP/src/main.py"
    STUB_PY="$TMP/stub.py"
    [ -f "$TMP/src/stub.py" ] && STUB_PY="$TMP/src/stub.py"
    python3 - "$OSINSTALL_PY" "$MAIN_PY" "$STUB_PY" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
s = p.read_text()
s = s.replace("return self.min_size * 2", "return self.min_size")
s = s.replace(
    "shutil.copytree(self.firmware_package.path, base)",
    "shutil.copytree(self.firmware_package.path, base, dirs_exist_ok=True)",
)
old = r"""                zinfo = self.pkg.getinfo(image)
                if zinfo.file_size % (4 * 1024) != 0:
                    raise Exception("The size of the rootfs image file must be a multiple of 4KiB.")
                with self.pkg.open(image) as sfd, \
                    open(f"/dev/r{info.name}", "r+b") as dfd:
                    self.fdcopy(sfd, dfd, zinfo.file_size)"""
new = r"""                local_img = os.path.join(os.environ.get("REPO_BASE", "."), image)
                if os.path.exists(local_img):
                    fsize = os.stat(local_img).st_size
                    with open(local_img, "rb") as sfd, open(f"/dev/r{info.name}", "r+b") as dfd:
                        self.fdcopy(sfd, dfd, fsize)
                else:
                    zinfo = self.pkg.getinfo(image)
                    with self.pkg.open(image) as sfd, open(f"/dev/r{info.name}", "r+b") as dfd:
                        self.fdcopy(sfd, dfd, zinfo.file_size)"""
assert old in s, "Expected osinstall.py block not found"
p.write_text(s.replace(old, new, 1))

mp = pathlib.Path(sys.argv[2])
ms = mp.read_text()
old_repair = """        self.ins = stub.StubInstaller(self.sysinfo, self.dutil, self.osinfo)
        if not self.ins.check_existing_install(osi):
            op = "upgrade" if upgrade else "repair"
            p_error(   "The existing installation is missing files.")
            p_message(f"This tool can only {op} installations that completed the first")
            p_message( "stage of the installation process. If it was interrupted, please")
            p_message( "delete the partitions manually and reinstall from scratch.")
            return False"""
new_repair = """        self.ins = stub.StubInstaller(self.sysinfo, self.dutil, self.osinfo)
        if not self.ins.check_existing_install(osi):
            if not upgrade and osi.attached_partitions and len(osi.attached_partitions) >= 2 and os.path.exists("vendorfw"):
                p_progress("Resuming interrupted stage-1 OS image & ESP installation...")
                self.dutil.remount_rw(self.ins.osi.system)
                self.osins = osinstall.OSInstaller(self.dutil, self.data, self.data["os_list"][0])
                self.osins.load_package()
                self.osins.part_info = osi.attached_partitions[:len(self.osins.template["partitions"])]
                self.osins.efi_part = osi.attached_partitions[0]
                class _ExistingFW:
                    path = "vendorfw"
                self.osins.firmware_package = _ExistingFW()
                self.osins.install(self.ins)
                if os.path.exists("all_firmware.tar.gz"):
                    for tdir in self.osins.idata_targets:
                        shutil.copy("all_firmware.tar.gz", os.path.join(tdir, "all_firmware.tar.gz"))
            else:
                op = "upgrade" if upgrade else "repair"
                p_error(   "The existing installation is missing files.")
                p_message(f"This tool can only {op} installations that completed the first")
                p_message( "stage of the installation process. If it was interrupted, please")
                p_message( "delete the partitions manually and reinstall from scratch.")
                return False"""
assert old_repair in ms, "Expected main.py repair block not found"
mp.write_text(ms.replace(old_repair, new_repair, 1))

if len(sys.argv) > 3:
    sp = pathlib.Path(sys.argv[3])
    if sp.exists():
        ss = sp.read_text()
        old_stub = """        if merge_stub_info:
            with open(os.path.join(path, "stub_info.json"), "r") as fd:
                stub_info = json.load(fd)
            stub_info.update(self.stub_info)
        else:
            stub_info = self.stub_info"""
        new_stub = """        stub_file = os.path.join(path, "stub_info.json")
        if merge_stub_info and os.path.exists(stub_file):
            try:
                with open(stub_file, "r") as fd:
                    stub_info = json.load(fd)
                stub_info.update(self.stub_info)
            except Exception:
                stub_info = self.stub_info
        else:
            stub_info = self.stub_info"""
        if old_stub in ss:
            sp.write_text(ss.replace(old_stub, new_stub, 1))
PY
    for m in "/Volumes/EFI - GOOGL/asahi" "/Volumes/EFI/asahi"; do
        if [ -d "$m" ] && [ ! -f "$m/stub_info.json" ]; then
            echo "{}" > "$m/stub_info.json" 2>/dev/null || true
        fi
    done
    sudo nvram -d upgrade-boot-volume 2>/dev/null || true
    sudo nvram -d IDInstallerDataV2 2>/dev/null || true
    EFI_NODE="$(diskutil list | awk '/EFI - GOOGL/{print $NF}')"
    if [ -n "$EFI_NODE" ]; then
        echo "[1.5/2] Syncing u-boot-nodtb.bin and m1n1 payloads directly into EFI ($EFI_NODE)..."
        MOUNT_DIR="/tmp/efi-mount-sync"
        mkdir -p "$MOUNT_DIR"
        sudo mount -t msdos "/dev/$EFI_NODE" "$MOUNT_DIR" 2>/dev/null || true
        if [ -d "$MOUNT_DIR" ]; then
            mkdir -p "$MOUNT_DIR/m1n1" "$MOUNT_DIR/gbos"
            cp -f "$DIR/esp/u-boot-nodtb.bin" "$MOUNT_DIR/u-boot-nodtb.bin"
            cp -f "$DIR/esp/m1n1/boot.bin" "$MOUNT_DIR/m1n1/boot.bin"
            cp -f "$DIR/esp/gbos/Image" "$MOUNT_DIR/gbos/Image"
            cp -f "$DIR/esp/gbos/initrd.img" "$MOUNT_DIR/gbos/initrd.img"
            cp -f "$DIR/esp/gbos/boot.cmd" "$MOUNT_DIR/gbos/boot.cmd"
            sync
            sudo umount "$MOUNT_DIR" 2>/dev/null || true
            rmdir "$MOUNT_DIR" 2>/dev/null || true
            echo "EFI partition successfully synced with u-boot-nodtb.bin and m1n1/boot.bin!"
        fi
    fi
    export REPO_BASE="$DIR"
    echo "[2/2] Launching interactive Asahi Linux installer with Googlebook OS catalog..."
    exec caffeinate -dis sudo -E ./install.sh
fi

echo "Usage:"
echo "  ./work/asahi-installer/install-asahi-baremetal.sh --install"
echo ""
echo "What --install does:"
echo "  1. Packages the ESP + googlebook.raw container into a zero-compression zip"
echo "     archive at $DIR/os/gbos-asahi-full.zip."
echo "  2. Downloads the official Asahi Linux installer into /tmp/asahi-install,"
echo "     injects the Googlebook OS installer_data.json manifest, sets REPO_BASE"
echo "     to $DIR, and launches the interactive Asahi partitioner & installer."
INSTALLER
chmod +x "$OUT/install-asahi-baremetal.sh"

say "Asahi bare-metal installer bundle ready: $OUT"
ls -lh "$OUT" "$OUT/os"
