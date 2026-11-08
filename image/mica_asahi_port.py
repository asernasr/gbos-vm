"""Bare-metal Apple Silicon (M1 MacBookAir10,1 / t8103 / j313) vendor overlay port.

Installs:
 1. Mesa 26.2.4 Asahi AGX Gallium GLES (`libEGL_asahi.so`, `libGLESv1_CM_asahi.so`,
    `libGLESv2_asahi.so`, `libgallium_dri.so`) + Honeykrisp Vulkan (`vulkan.asahi.so`).
 2. Apple DCP + Asahi AGX `minigbm` scanout alignment & format shim (`libasahi_dcp_bm.so`),
    wired as the first `DT_NEEDED` dependency of `libminigbm_gralloc.so` to enforce
    128-byte (`AIL_CACHELINE`) row pitch alignment, 16 KiB (`apple-dart` IOMMU) page
    size alignment, and Apple DCP scanout format filtering (`XR24`/`AR24`/`AB24`/`XB24`).
 3. Apple Silicon Speaker Protection & Audio DSP Limiter daemon (`asahi-speakersafetyd`)
    for `snd-soc-macaudio` + `TAS2764` V/I-sense thermal feedback + `CS42L83` jack routing.
 4. Bare-metal hardware bridge daemon (`asahi-hw-daemon`) for Broadcom `BCM4377` Bluetooth
    (`hci0` -> `/dev/asahi_bt_hci`), `apple-panel-bl` backlight, `macsmc-battery` telemetry,
    and `SW_LID` / `SW_HEADPHONE_INSERT` switch events.
 5. Apple SPI Trackpad & Keyboard `.idc` and `.kl` input configurations (pointer mode +
    multi-touch gestures + macOS media/brightness keys).
 6. Workstation root PTY multiplex daemon (`workstation-shell`) and `linux-env` package
    manager / container helper (`/data/asahi-linux`).
"""

from pathlib import Path
import os
import struct

R = Path(__file__).resolve().parents[1]


def patch_minigbm_dt_needed(minigbm_bytes: bytes) -> bytes:
    """Replace unused DT_NEEDED 'libnativewindow.so' (18 chars) in libminigbm_gralloc.so
    with 'libasahi_dcp_bm.so' (18 chars) and move it to the first DT_NEEDED slot so
    linker64 loads libasahi_dcp_bm.so before resolving libminigbm_gralloc.so symbols.
    """
    b = bytearray(minigbm_bytes)
    old_name = b"libnativewindow.so\x00"
    new_name = b"libasahi_dcp_bm.so\x00"
    assert len(old_name) == len(new_name) == 19

    # Locate PT_DYNAMIC and DT_STRTAB
    e_phoff = struct.unpack_from("<Q", b, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", b, 54)
    dyn_off = dyn_sz = 0
    for i in range(e_phnum):
        p_type, _, p_offset, _, _, p_filesz, _, _ = struct.unpack_from(
            "<IIQQQQQQ", b, e_phoff + i * e_phentsize
        )
        if p_type == 2:  # PT_DYNAMIC
            dyn_off, dyn_sz = p_offset, p_filesz
            break
    assert dyn_off > 0

    strtab_off = 0
    needed_indices = []
    n_slots = dyn_sz // 16
    for j in range(n_slots):
        tag, val = struct.unpack_from("<QQ", b, dyn_off + j * 16)
        if tag == 5:  # DT_STRTAB
            strtab_off = val
        elif tag == 1:  # DT_NEEDED
            needed_indices.append((j, val))
        elif tag == 0:
            break
    assert strtab_off > 0 and needed_indices

    target_slot = None
    target_val = None
    for slot_idx, str_rel in needed_indices:
        s_off = strtab_off + str_rel
        if bytes(b[s_off : s_off + len(old_name)]) == old_name:
            b[s_off : s_off + len(new_name)] = new_name
            target_slot = slot_idx
            target_val = str_rel
            break
    assert target_slot is not None, "libnativewindow.so DT_NEEDED not found in libminigbm_gralloc.so"

    # Shift DT_NEEDED entries [0 .. target_slot] so libasahi_dcp_bm.so is slot 0
    first_slot = needed_indices[0][0]
    if target_slot != first_slot:
        prev_entries = [
            struct.unpack_from("<QQ", b, dyn_off + k * 16)
            for k in range(first_slot, target_slot)
        ]
        struct.pack_into("<QQ", b, dyn_off + first_slot * 16, 1, target_val)
        for idx, (t, v) in enumerate(prev_entries):
            struct.pack_into("<QQ", b, dyn_off + (first_slot + 1 + idx) * 16, t, v)

    return bytes(b)


APPLE_TRACKPAD_IDC = b"""# Apple Silicon SPI / MTP Multi-Touch Force Touch Trackpad Configuration
# Configures desktop cursor navigation and multi-finger trackpad gestures.
touch.deviceType = pointer
touch.orientationAware = 1
touch.gestureMode = pointer
cursor.mode = navigation
cursor.orientationAware = 1
touch.pressure.scale = 0.004
touch.size.calibration = geometric
"""

APPLE_KEYBOARD_KL = b"""# Apple Silicon SPI Built-in Keyboard Layout (MacBookAir10,1 / j313)
key 1     ESCAPE
key 2     1
key 3     2
key 4     3
key 5     4
key 6     5
key 7     6
key 8     7
key 9     8
key 10    9
key 11    0
key 12    MINUS
key 13    EQUALS
key 14    DEL
key 15    TAB
key 16    Q
key 17    W
key 18    E
key 19    R
key 20    T
key 21    Y
key 22    U
key 23    I
key 24    O
key 25    P
key 26    LEFT_BRACKET
key 27    RIGHT_BRACKET
key 28    ENTER
key 29    CTRL_LEFT
key 30    A
key 31    S
key 32    D
key 33    F
key 34    G
key 35    H
key 36    J
key 37    K
key 38    L
key 39    SEMICOLON
key 40    APOSTROPHE
key 41    GRAVE
key 42    SHIFT_LEFT
key 43    BACKSLASH
key 44    Z
key 45    X
key 46    C
key 47    V
key 48    B
key 49    N
key 50    M
key 51    COMMA
key 52    PERIOD
key 53    SLASH
key 54    SHIFT_RIGHT
key 56    ALT_LEFT
key 57    SPACE
key 58    CAPS_LOCK
key 59    F1
key 60    F2
key 61    F3
key 62    F4
key 63    F5
key 64    F6
key 65    F7
key 66    F8
key 67    F9
key 68    F10
key 87    F11
key 88    F12
key 100   ALT_RIGHT
key 102   MOVE_HOME
key 103   DPAD_UP
key 104   PAGE_UP
key 105   DPAD_LEFT
key 106   DPAD_RIGHT
key 107   MOVE_END
key 108   DPAD_DOWN
key 109   PAGE_DOWN
key 111   FORWARD_DEL
key 113   VOLUME_MUTE
key 114   VOLUME_DOWN
key 115   VOLUME_UP
key 116   POWER
key 125   META_LEFT
key 126   META_RIGHT
key 163   MEDIA_NEXT
key 164   MEDIA_PLAY_PAUSE
key 165   MEDIA_PREVIOUS
key 217   SEARCH
key 224   BRIGHTNESS_DOWN
key 225   BRIGHTNESS_UP
key 464   FUNCTION
"""

LINUX_ENV_SCRIPT = b"""#!/system/bin/sh
# Googlebook OS on Apple Silicon - Linux Workstation & Package Manager Helper
# Usage:
#   linux-env --bootstrap [arch|debian]  : Populate /data/asahi-linux rootfs
#   linux-env [command...]               : Enter persistent root Linux workstation shell

ROOTFS="/data/asahi-linux"

if [ "$1" = "--bootstrap" ]; then
    DISTRO="${2:-arch}"
    mkdir -p "$ROOTFS/bin" "$ROOTFS/usr/bin" "$ROOTFS/usr/local/bin" \
             "$ROOTFS/etc" "$ROOTFS/root" "$ROOTFS/tmp" "$ROOTFS/var/lib/pacman" \
             "$ROOTFS/proc" "$ROOTFS/sys" "$ROOTFS/dev"
    echo "nameserver 1.1.1.1" > "$ROOTFS/etc/resolv.conf"
    echo "nameserver 8.8.8.8" >> "$ROOTFS/etc/resolv.conf"
    cat > "$ROOTFS/etc/profile" <<'EOF'
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/system/bin:/vendor/bin"
export TERM="${TERM:-xterm-256color}"
export HOME="/root"
export PS1='\\[\\033[01;32m\\]root@gbos-asahi\\[\\033[00m\\]:\\[\\033[01;34m\\]\\w\\[\\033[00m\\]# '
EOF
    if [ ! -e "$ROOTFS/bin/sh" ]; then
        cp -f /system/bin/sh "$ROOTFS/bin/sh" 2>/dev/null || ln -sf /system/bin/sh "$ROOTFS/bin/sh"
    fi
    echo "[linux-env] Initialized persistent workstation root at $ROOTFS (distro=$DISTRO)."
    echo "[linux-env] To unpack an Arch Linux ARM or Debian arm64 rootfs tarball:"
    echo "  tar -xpf ArchLinuxARM-aarch64-latest.tar.gz -C $ROOTFS --numeric-owner"
    exit 0
fi

if [ "$#" -gt 0 ]; then
    exec /vendor/bin/workstation-shell --linux "$@"
else
    exec /vendor/bin/workstation-shell --linux
fi
"""

ASAHI_BAREMETAL_RC = b"""# Bare-Metal Apple Silicon (MacBookAir10,1 / t8103 / j313) Init Configuration

on early-init
    # Ensure DRM/KMS (apple-dcp card0 + drm-asahi renderD128) and ALSA permissions
    mkdir /dev/dri 0755 system graphics
    chmod 0666 /dev/dri/card0
    chmod 0666 /dev/dri/card1
    chmod 0666 /dev/dri/renderD128
    chmod 0666 /dev/dri/renderD129

on init
    # Label Apple Silicon SoC GPU, DCP, Backlight, and SMC sysfs nodes
    restorecon_recursive /sys/devices/platform/soc
    restorecon_recursive /sys/class/backlight
    restorecon_recursive /sys/class/power_supply
    restorecon_recursive /sys/class/drm
    setprop vendor.asahi.display.brightness 180
    start asahi-speakersafetyd
    start asahi-hw-daemon
    start asahi-workstation-daemon

on post-fs-data
    mkdir /data/asahi-linux 0755 root root
    mkdir /data/local/root-home 0700 root root

on property:vendor.asahi.bt.ready=1
    start vendor.bluetooth-aidl-qti

# Real-time TAS2764 V/I-sense thermal model + 125 Hz HPF + j313 EQ + lookahead limiter
service asahi-speakersafetyd /vendor/bin/asahi-speakersafetyd
    class core
    user root
    group root audio system
    seclabel u:r:init:s0

# Apple Silicon hardware bridge: BCM4377 BT HCI PTY, apple-panel-bl, macsmc-battery, lid/jack
service asahi-hw-daemon /vendor/bin/asahi-hw-daemon
    class core
    user root
    group root bluetooth system graphics input wakelock
    seclabel u:r:init:s0

# Workstation root PTY multiplex daemon (/dev/socket/asahi_workstation)
service asahi-workstation-daemon /vendor/bin/workstation-shell --daemon
    class core
    user root
    group root shell system
    seclabel u:r:init:s0
"""


def apply(add, original, files):
    guest_dir = (
        Path(os.environ["GOOGLEBOOK_GUEST_DIR"])
        if os.environ.get("GOOGLEBOOK_GUEST_DIR")
        else R / "work/guest"
    )
    mesa_asahi_dir = guest_dir / "mesa-asahi"
    asahi_bin_dir = guest_dir / "asahi-bin"

    # 1. Install Asahi AGX GLES + Honeykrisp Vulkan libraries
    for lib_name in [
        "libEGL_asahi.so",
        "libGLESv1_CM_asahi.so",
        "libGLESv2_asahi.so",
    ]:
        add(
            f"lib64/egl/{lib_name}",
            (mesa_asahi_dir / lib_name).read_bytes(),
            "same_process_hal_file",
        )
    add(
        "lib64/libgallium_dri.so",
        (mesa_asahi_dir / "libgallium_dri.so").read_bytes(),
        "same_process_hal_file",
    )
    add(
        "lib64/libdrm.so",
        (mesa_asahi_dir / "libdrm.so").read_bytes(),
        "same_process_hal_file",
    )
    add(
        "lib64/hw/vulkan.asahi.so",
        (mesa_asahi_dir / "libvulkan_asahi.so").read_bytes(),
        "same_process_hal_file",
    )

    # 2. Install Apple DCP + Asahi AGX minigbm alignment/format shim and patch libminigbm_gralloc.so
    add(
        "lib64/libasahi_dcp_bm.so",
        (asahi_bin_dir / "libasahi_dcp_bm.so").read_bytes(),
        "same_process_hal_file",
    )
    assert "lib64/libminigbm_gralloc.so" in files, "--graphics must run before --asahi-baremetal"
    orig_minigbm, mg_label, mg_mode = files["lib64/libminigbm_gralloc.so"]
    patched_minigbm = patch_minigbm_dt_needed(orig_minigbm)
    add("lib64/libminigbm_gralloc.so", patched_minigbm, mg_label, mg_mode)

    # 3. Install bare-metal daemons & workstation tools
    add(
        "bin/asahi-speakersafetyd",
        (asahi_bin_dir / "asahi-speakersafetyd").read_bytes(),
        "vendor_file",
        0o755,
    )
    add(
        "bin/asahi-hw-daemon",
        (asahi_bin_dir / "asahi-hw-daemon").read_bytes(),
        "vendor_file",
        0o755,
    )
    add(
        "bin/workstation-shell",
        (asahi_bin_dir / "workstation-shell").read_bytes(),
        "vendor_file",
        0o755,
    )
    add("bin/linux-env", LINUX_ENV_SCRIPT, "vendor_shell_exec", 0o755)
    add("etc/init/asahi-baremetal.rc", ASAHI_BAREMETAL_RC, "vendor_configs_file")

    # 4. Install Apple SPI Trackpad & Keyboard input configs (.idc & .kl)
    for idc_name in [
        "Apple_SPI_Trackpad.idc",
        "Apple_MTP_multi-touch.idc",
        "Apple_Internal_Keyboard___Trackpad.idc",
    ]:
        add(f"usr/idc/{idc_name}", APPLE_TRACKPAD_IDC, "vendor_configs_file")
    for kl_name in [
        "Apple_SPI_Keyboard.kl",
        "Apple_Internal_Keyboard___Trackpad.kl",
    ]:
        add(f"usr/keylayout/{kl_name}", APPLE_KEYBOARD_KL, "vendor_configs_file")

    # 5. Update Bluetooth HAL service to accept either /dev/asahi_bt_hci (bare-metal BCM4377) or /dev/hvc0
    # and strip QEMU-only `console ttyAMA0` from init .rc services (such as vm-offline-desktop.rc)
    # so Android init never aborts service launch on bare-metal Apple Silicon where PL011 ttyAMA0 is absent.
    bt_svc = "android.hardware.bluetooth@aidl-service-qti"
    if f"etc/init/{bt_svc}.rc" in files:
        bt_rc, bt_lbl, bt_mode = files[f"etc/init/{bt_svc}.rc"]
        bt_rc = bt_rc.replace(b"--serial /dev/hvc0", b"--serial /dev/asahi_bt_hci")
        add(f"etc/init/{bt_svc}.rc", bt_rc, bt_lbl, bt_mode)

    for rc_path in list(files.keys()):
        if rc_path.startswith("etc/init/") and rc_path.endswith(".rc"):
            rc_bytes, rc_lbl, rc_mode = files[rc_path]
            if b"    console ttyAMA0\n" in rc_bytes:
                add(rc_path, rc_bytes.replace(b"    console ttyAMA0\n", b""), rc_lbl, rc_mode)

    # 6. Switch vendor build.prop to Asahi AGX GLES + Honeykrisp Vulkan + bare-metal properties
    b_prop, b_lbl, b_mode = files["build.prop"]
    b_prop = b_prop.replace(b"ro.hardware.egl=virgl\n", b"ro.hardware.egl=asahi\n")
    b_prop = b_prop.replace(b"ro.hardware.vulkan=none\n", b"ro.hardware.vulkan=asahi\n")
    b_prop = b_prop.replace(b"ro.hardware.vulkan=virtio\n", b"ro.hardware.vulkan=asahi\n")
    b_prop += (
        b"\n# Bare-metal Apple Silicon (M1 / t8103 / j313) hardware configuration\n"
        b"ro.boot.product.vendor.sku=vmbt\n"
        b"wifi.interface=wlan0\n"
        b"ro.surface_flinger.max_frame_buffer_acquired_buffers=3\n"
        b"ro.surface_flinger.primary_display_orientation=ORIENTATION_0\n"
    )
    add("build.prop", b_prop, b_lbl, b_mode)

    # 7. Label Apple Silicon sysfs & device nodes in vendor_file_contexts and ueventd.rc
    ctx, ctx_lbl, ctx_mode = files["etc/selinux/vendor_file_contexts"]
    ctx += (
        b"\n# Bare-metal Apple Silicon (t8103) GPU, DCP, Backlight, SMC & BT PTY contexts\n"
        b"/sys/devices/platform/soc(/.*)? u:object_r:sysfs_gpu:s0\n"
        b"/sys/class/drm(/.*)? u:object_r:sysfs_gpu:s0\n"
        b"/dev/asahi_bt_hci u:object_r:hci_attach_dev:s0\n"
        b"/dev/socket/asahi_workstation u:object_r:socket_device:s0\n"
    )
    add("etc/selinux/vendor_file_contexts", ctx, ctx_lbl, ctx_mode)

    ueventd_orig = (
        files["etc/ueventd.rc"][0]
        if "etc/ueventd.rc" in files
        else original("etc/ueventd.rc")
    )
    ueventd_orig += (
        b"\n# Bare-metal Apple Silicon DRM, ALSA, Backlight & Input permissions\n"
        b"/dev/dri/card0            0666   system     graphics\n"
        b"/dev/dri/card1            0666   system     graphics\n"
        b"/dev/dri/renderD128       0666   system     graphics\n"
        b"/dev/dri/renderD129       0666   system     graphics\n"
        b"/dev/asahi_bt_hci         0660   bluetooth  bluetooth\n"
        b"/dev/snd/*                0660   system     audio\n"
        b"/dev/input/*              0660   root       input\n"
    )
    add("etc/ueventd.rc", ueventd_orig, "vendor_configs_file")

    return ["usr", "usr/idc"], ["usr/keylayout"]
