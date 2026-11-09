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


def patch_wifi_hal_for_brcmfmac(wifi_hal_bytes: bytes) -> bytes:
    """Adapt stock /vendor/lib64/libwifi-hal.so so DriverTool and wifi_load_driver succeed
    on Apple Silicon's always-loaded Broadcom brcmfmac (cfg80211/nl80211 wlan0) without
    requiring Qualcomm's /dev/wlan or /sys/module/wlan/parameters/fwpath nodes.
    """
    b = bytearray(wifi_hal_bytes)
    MOV_W0_0 = 0x52800000
    MOV_W0_1 = 0x52800020
    RET = 0xD65F03C0
    patches = [
        # DriverTool C++ methods (return bool: 1=true, 0=false)
        (0x34B30, MOV_W0_1),  # DriverTool::LoadDriver() -> true
        (0x34B50, MOV_W0_1),  # DriverTool::UnloadDriver() -> true
        (0x34B70, MOV_W0_1),  # DriverTool::IsDriverLoaded() -> true
        (0x34B90, MOV_W0_0),  # DriverTool::IsFirmwareModeChangeNeeded(int) -> false
        (0x34BB0, MOV_W0_1),  # DriverTool::ChangeFirmwareMode(int) -> true
        # Legacy C driver state helpers (return 0=success or 1=loaded)
        (0x909C0, MOV_W0_0),  # wifi_change_driver_state(const char*) -> 0
        (0x90CE0, MOV_W0_1),  # is_wifi_driver_loaded() -> 1
        (0x90D60, MOV_W0_0),  # wifi_load_driver() -> 0
        (0x90E10, MOV_W0_0),  # wifi_unload_driver() -> 0
        (0x90F10, MOV_W0_0),  # wifi_change_fw_path(const char*) -> 0
    ]
    for off, mov_insn in patches:
        struct.pack_into("<II", b, off, mov_insn, RET)
    return bytes(b)


def patch_lights_hal_for_apple_backlight(lights_hal_bytes: bytes) -> bytes:
    """Patch /vendor/bin/hw/android.hardware.lights-service.android-desktop so native
    Android Display Brightness, Screen Dimming, Lockscreen Sleep, and Screen Timeout
    drive Apple Silicon's /sys/class/backlight/apple-panel-bl/ directly.
    """
    old_lcd = b"/sys/class/backlight/backlight-lcd0/"
    new_lcd = b"/sys/class/backlight/apple-panel-bl/"
    assert len(old_lcd) == len(new_lcd) == 36
    assert old_lcd in lights_hal_bytes
    lights_hal_bytes = lights_hal_bytes.replace(old_lcd, new_lcd)

    old_intel = b"/sys/class/backlight/intel_backlight/"
    new_apple = b"/sys/class/backlight/apple_backlight/"
    assert len(old_intel) == len(new_apple) == 37
    assert old_intel in lights_hal_bytes
    lights_hal_bytes = lights_hal_bytes.replace(old_intel, new_apple)
    return lights_hal_bytes


def patch_camera_hal_for_apple_isp(cam_hal_bytes: bytes) -> bytes:
    """Patch /vendor/bin/hw/android.hardware.camera.provider-usb-service.android-desktop
    so that Apple Silicon's built-in FaceTime HD Camera (/dev/video0, uevent line
    'DRIVER=apple-isp') takes the built-in non-USB V4L2 camera path (0x4e798, matching
    'vivid:vivid' in /vendor/etc/camera/internal_usb_camera_info.txtpb as FACING_FRONT)
    while preserving external USB UVC webcam ('PRODUCT=...') support.
    """
    b = bytearray(cam_hal_bytes)
    orig_insns = struct.unpack_from("<15I", b, 0x4E4BC)
    assert orig_insns[0] == 0xF100315F and orig_insns[14] == 0x140000A9
    new_insns = [
        0xF9004BEA,  # 4e4bc: str  x10, [sp, #0x90]
        0xF100315F,  # 4e4c0: cmp  x10, #0xc
        0x540001A3,  # 4e4c4: b.lo 0x4e4f8 (if len < 12, go to cmp x10, #8)
        0xF94002C8,  # 4e4c8: ldr  x8, [x22]
        0xD28A4889,  # 4e4cc: mov  x9, #0x5244 ('D','R')
        0xF2AAC929,  # 4e4d0: movk x9, #0x5649, lsl #16 ('I','V')
        0xF2CA48A9,  # 4e4d4: movk x9, #0x5245, lsl #32 ('E','R')
        0xF2EC27A9,  # 4e4d8: movk x9, #0x613d, lsl #48 ('=','a' -> "DRIVER=a")
        0xEB09011F,  # 4e4dc: cmp  x8, x9
        0x540015C0,  # 4e4e0: b.eq 0x4e798 (DRIVER=apple-isp -> built-in V4L2 camera)
        0xF2EEC7A9,  # 4e4e4: movk x9, #0x763d, lsl #48 ('=','v' -> "DRIVER=v")
        0xEB09011F,  # 4e4e8: cmp  x8, x9
        0x54001560,  # 4e4ec: b.eq 0x4e798 (DRIVER=vivid -> built-in V4L2 camera)
        0x14000004,  # 4e4f0: b    0x4e500 (otherwise check PRODUCT=)
        0xD503201F,  # 4e4f4: nop
    ]
    struct.pack_into("<15I", b, 0x4E4BC, *new_insns)
    return bytes(b)


APPLE_ISP_CAMERA_INFO_TXTPB = b"""
camera_modules {
  key: "vivid:vivid"
  value {
    lens_info {
      available_focal_lengths: 1.85
      available_apertures: 2.2
      minimum_focus_distance: 0.2
      optimal_focus_distance: 0.5
    }
    sensor_info {
      physical_size {
        width: 3.20
        height: 1.80
      }
      pixel_array_size {
        width: 1920
        height: 1080
      }
      active_array {
        left: 0
        top: 0
        right: 1920
        bottom: 1080
      }
    }
    enable_face_detection: true
    facing: FACING_FRONT
  }
}
"""

ASAHI_MEDIA_CODECS_HAMOA_XML = b"""<?xml version="1.0" encoding="utf-8" ?>
<MediaCodecs>
    <Include href="media_codecs_google_audio.xml" />
    <Include href="media_codecs_google_telephony.xml" />
    <Include href="media_codecs_google_c2.xml" />
</MediaCodecs>
"""

ASAHI_MEDIA_CODECS_GOOGLE_C2_VIDEO_XML = b"""<?xml version="1.0" encoding="utf-8" ?>
<!-- Apple Silicon (M1 Firestorm/Icestorm NEON) 4K60 / 1080p240 Codec2 Video Configuration -->
<Included>
    <Decoders>
        <MediaCodec name="c2.android.mpeg4.decoder" type="video/mp4v-es">
            <Alias name="OMX.google.mpeg4.decoder" />
            <Limit name="size" min="2x2" max="1920x1088" />
            <Limit name="alignment" value="2x2" />
            <Limit name="block-size" value="16x16" />
            <Limit name="blocks-per-second" range="12-489600" />
            <Limit name="bitrate" range="1-60000000" />
            <Feature name="adaptive-playback" />
        </MediaCodec>
        <MediaCodec name="c2.android.h263.decoder" type="video/3gpp">
            <Alias name="OMX.google.h263.decoder" />
            <Limit name="size" min="2x2" max="864x480" />
            <Limit name="alignment" value="2x2" />
            <Limit name="bitrate" range="1-16000000" />
            <Feature name="adaptive-playback" />
        </MediaCodec>
        <MediaCodec name="c2.android.avc.decoder" type="video/avc">
            <Alias name="OMX.google.h264.decoder" />
            <Limit name="size" min="2x2" max="4096x4096" />
            <Limit name="alignment" value="2x2" />
            <Limit name="block-size" value="16x16" />
            <Limit name="block-count" range="1-36864" />
            <Limit name="blocks-per-second" range="1-2211840" />
            <Limit name="bitrate" range="1-160000000" />
            <Limit name="frame-rate" range="1-240" />
            <Feature name="adaptive-playback" />
            <Limit name="performance-point-3840x2160" value="60" />
            <Limit name="performance-point-2560x1440" value="120" />
            <Limit name="performance-point-1920x1080" value="240" />
            <Limit name="performance-point-1280x720" value="240" />
        </MediaCodec>
        <MediaCodec name="c2.android.hevc.decoder" type="video/hevc">
            <Alias name="OMX.google.hevc.decoder" />
            <Limit name="size" min="2x2" max="4096x4096" />
            <Limit name="alignment" value="2x2" />
            <Limit name="block-size" value="8x8" />
            <Limit name="block-count" range="1-196608" />
            <Limit name="blocks-per-second" range="1-8847360" />
            <Limit name="bitrate" range="1-160000000" />
            <Limit name="frame-rate" range="1-240" />
            <Feature name="adaptive-playback" />
            <Limit name="performance-point-3840x2160" value="60" />
            <Limit name="performance-point-2560x1440" value="120" />
            <Limit name="performance-point-1920x1080" value="240" />
            <Limit name="performance-point-1280x720" value="240" />
        </MediaCodec>
        <MediaCodec name="c2.android.vp8.decoder" type="video/x-vnd.on2.vp8">
            <Alias name="OMX.google.vp8.decoder" />
            <Limit name="size" min="2x2" max="4096x4096" />
            <Limit name="alignment" value="1x1" />
            <Limit name="block-size" value="16x16" />
            <Limit name="block-count" range="1-36864" />
            <Limit name="blocks-per-second" range="1-2211840" />
            <Limit name="bitrate" range="1-100000000" />
            <Limit name="frame-rate" range="1-240" />
            <Feature name="adaptive-playback" />
            <Limit name="performance-point-3840x2160" value="60" />
            <Limit name="performance-point-1920x1080" value="240" />
        </MediaCodec>
        <MediaCodec name="c2.android.vp9.decoder" type="video/x-vnd.on2.vp9">
            <Alias name="OMX.google.vp9.decoder" />
            <Limit name="size" min="2x2" max="4096x4096" />
            <Limit name="alignment" value="1x1" />
            <Limit name="block-size" value="16x16" />
            <Limit name="block-count" range="1-36864" />
            <Limit name="blocks-per-second" range="1-2211840" />
            <Limit name="bitrate" range="1-160000000" />
            <Limit name="frame-rate" range="1-240" />
            <Feature name="adaptive-playback" />
            <Limit name="performance-point-3840x2160" value="60" />
            <Limit name="performance-point-2560x1440" value="120" />
            <Limit name="performance-point-1920x1080" value="240" />
            <Limit name="performance-point-1280x720" value="240" />
        </MediaCodec>
        <MediaCodec name="c2.android.av1.decoder" type="video/av01" variant="slow-cpu,!slow-cpu">
            <Limit name="alignment" value="1x1" />
            <Limit name="block-size" value="16x16" />
            <Variant name="!slow-cpu">
                <Limit name="size" min="2x2" max="4096x4096" />
                <Limit name="block-count" range="1-36864" />
                <Limit name="blocks-per-second" range="1-2211840" />
                <Limit name="bitrate" range="1-160000000" />
            </Variant>
            <Variant name="slow-cpu">
                <Limit name="size" min="2x2" max="4096x4096" />
                <Limit name="block-count" range="1-36864" />
                <Limit name="blocks-per-second" range="1-2211840" />
                <Limit name="bitrate" range="1-160000000" />
            </Variant>
            <Feature name="adaptive-playback" />
            <Limit name="performance-point-3840x2160" value="60" />
            <Limit name="performance-point-2560x1440" value="120" />
            <Limit name="performance-point-1920x1080" value="240" />
            <Limit name="performance-point-1280x720" value="240" />
            <Attribute name="software-codec" />
        </MediaCodec>
        <MediaCodec name="c2.android.av1-dav1d.decoder" type="video/av01">
            <Limit name="size" min="2x2" max="4096x4096" />
            <Limit name="alignment" value="1x1" />
            <Limit name="block-size" value="16x16" />
            <Limit name="block-count" range="1-36864" />
            <Limit name="blocks-per-second" range="1-2211840" />
            <Limit name="bitrate" range="1-160000000" />
            <Feature name="adaptive-playback" />
            <Limit name="performance-point-3840x2160" value="60" />
            <Limit name="performance-point-2560x1440" value="120" />
            <Limit name="performance-point-1920x1080" value="240" />
            <Limit name="performance-point-1280x720" value="240" />
            <Attribute name="software-codec" />
        </MediaCodec>
    </Decoders>
    <Encoders>
        <MediaCodec name="c2.android.h263.encoder" type="video/3gpp">
            <Alias name="OMX.google.h263.encoder" />
            <Limit name="size" min="176x144" max="176x144" />
            <Limit name="alignment" value="16x16" />
            <Limit name="bitrate" range="1-128000" />
        </MediaCodec>
        <MediaCodec name="c2.android.avc.encoder" type="video/avc">
            <Alias name="OMX.google.h264.encoder" />
            <Limit name="size" min="16x16" max="3840x2160" />
            <Limit name="alignment" value="2x2" />
            <Limit name="block-size" value="16x16" />
            <Limit name="block-count" range="1-32400" />
            <Limit name="blocks-per-second" range="1-972000" />
            <Limit name="bitrate" range="1-48000000" />
            <Feature name="intra-refresh" />
        </MediaCodec>
        <MediaCodec name="c2.android.hevc.encoder" type="video/hevc">
            <Limit name="size" min="2x2" max="1920x1080" />
            <Limit name="alignment" value="2x2" />
            <Limit name="block-size" value="8x8" />
            <Limit name="block-count" range="1-32400" />
            <Limit name="blocks-per-second" range="1-972000" />
            <Limit name="frame-rate" range="1-120" />
            <Limit name="bitrate" range="1-40000000" />
            <Limit name="complexity" range="0-10" default="0" />
            <Limit name="quality" range="0-100" default="80" />
            <Feature name="bitrate-modes" value="VBR,CBR,CQ" />
        </MediaCodec>
        <MediaCodec name="c2.android.mpeg4.encoder" type="video/mp4v-es">
            <Alias name="OMX.google.mpeg4.encoder" />
            <Limit name="size" min="16x16" max="176x144" />
            <Limit name="alignment" value="16x16" />
            <Limit name="block-size" value="16x16" />
            <Limit name="blocks-per-second" range="12-1485" />
            <Limit name="bitrate" range="1-64000" />
        </MediaCodec>
        <MediaCodec name="c2.android.vp8.encoder" type="video/x-vnd.on2.vp8">
            <Alias name="OMX.google.vp8.encoder" />
            <Limit name="size" min="2x2" max="3840x2160" />
            <Limit name="alignment" value="2x2" />
            <Limit name="block-size" value="16x16" />
            <Limit name="block-count" range="1-32400" />
            <Limit name="bitrate" range="1-40000000" />
            <Feature name="bitrate-modes" value="VBR,CBR" />
        </MediaCodec>
        <MediaCodec name="c2.android.vp9.encoder" type="video/x-vnd.on2.vp9">
            <Alias name="OMX.google.vp9.encoder" />
            <Limit name="size" min="2x2" max="3840x2160" />
            <Limit name="alignment" value="2x2" />
            <Limit name="block-size" value="16x16" />
            <Limit name="block-count" range="1-32400" />
            <Limit name="bitrate" range="1-40000000" />
            <Feature name="bitrate-modes" value="VBR,CBR" />
        </MediaCodec>
    </Encoders>
</Included>
"""

ASAHI_MEDIA_CODECS_PERFORMANCE_XML = b"""<?xml version="1.0" encoding="utf-8" ?>
<MediaCodecs>
    <Decoders>
        <MediaCodec name="c2.android.avc.decoder" type="video/avc" update="true">
            <Limit name="measured-frame-rate-320x240" range="400-900" />
            <Limit name="measured-frame-rate-720x480" range="300-600" />
            <Limit name="measured-frame-rate-1280x720" range="240-500" />
            <Limit name="measured-frame-rate-1920x1080" range="120-300" />
            <Limit name="measured-frame-rate-3840x2160" range="60-120" />
        </MediaCodec>
        <MediaCodec name="c2.android.hevc.decoder" type="video/hevc" update="true">
            <Limit name="measured-frame-rate-352x288" range="644-1415" />
            <Limit name="measured-frame-rate-640x360" range="454-998" />
            <Limit name="measured-frame-rate-720x480" range="353-776" />
            <Limit name="measured-frame-rate-1280x720" range="240-518" />
            <Limit name="measured-frame-rate-1920x1080" range="185-401" />
            <Limit name="measured-frame-rate-3840x2160" range="60-140" />
        </MediaCodec>
        <MediaCodec name="c2.android.vp8.decoder" type="video/x-vnd.on2.vp8" update="true">
            <Limit name="measured-frame-rate-320x180" range="550-1850" />
            <Limit name="measured-frame-rate-640x360" range="250-800" />
            <Limit name="measured-frame-rate-1280x720" range="200-400" />
            <Limit name="measured-frame-rate-1920x1080" range="120-240" />
            <Limit name="measured-frame-rate-3840x2160" range="60-120" />
        </MediaCodec>
        <MediaCodec name="c2.android.vp9.decoder" type="video/x-vnd.on2.vp9" update="true">
            <Limit name="measured-frame-rate-320x180" range="488-1500" />
            <Limit name="measured-frame-rate-640x360" range="265-900" />
            <Limit name="measured-frame-rate-1280x720" range="240-450" />
            <Limit name="measured-frame-rate-1920x1080" range="120-260" />
            <Limit name="measured-frame-rate-2560x1440" range="90-180" />
            <Limit name="measured-frame-rate-3840x2160" range="60-120" />
        </MediaCodec>
        <MediaCodec name="c2.android.av1.decoder" type="video/av01" update="true">
            <Limit name="measured-frame-rate-352x288" range="535-1177" />
            <Limit name="measured-frame-rate-640x360" range="351-771" />
            <Limit name="measured-frame-rate-720x480" range="293-644" />
            <Limit name="measured-frame-rate-1280x720" range="240-480" />
            <Limit name="measured-frame-rate-1920x1080" range="120-240" />
            <Limit name="measured-frame-rate-2560x1440" range="90-180" />
            <Limit name="measured-frame-rate-3840x2160" range="60-120" />
        </MediaCodec>
        <MediaCodec name="c2.android.av1-dav1d.decoder" type="video/av01" update="true">
            <Limit name="measured-frame-rate-352x288" range="1199-2637" />
            <Limit name="measured-frame-rate-640x360" range="803-1405" />
            <Limit name="measured-frame-rate-720x480" range="621-1441" />
            <Limit name="measured-frame-rate-1280x720" range="303-741" />
            <Limit name="measured-frame-rate-1920x1080" range="160-360" />
            <Limit name="measured-frame-rate-2560x1440" range="90-180" />
            <Limit name="measured-frame-rate-3840x2160" range="60-120" />
        </MediaCodec>
        <MediaCodec name="c2.android.h263.decoder" type="video/3gpp" update="true">
            <Limit name="measured-frame-rate-176x144" range="800-2600" />
            <Limit name="measured-frame-rate-352x288" range="620-1900" />
        </MediaCodec>
        <MediaCodec name="c2.android.mpeg4.decoder" type="video/mp4v-es" update="true">
            <Limit name="measured-frame-rate-176x144" range="865-2800" />
            <Limit name="measured-frame-rate-480x360" range="728-1564" />
            <Limit name="measured-frame-rate-1280x720" range="319-714" />
        </MediaCodec>
    </Decoders>
    <Encoders>
        <MediaCodec name="c2.android.h263.encoder" type="video/3gpp" update="true">
            <Limit name="measured-frame-rate-176x144" range="693-1401" />
        </MediaCodec>
        <MediaCodec name="c2.android.avc.encoder" type="video/avc" update="true">
            <Limit name="measured-frame-rate-320x240" range="240-980" />
            <Limit name="measured-frame-rate-720x480" range="256-404" />
            <Limit name="measured-frame-rate-1280x720" range="120-240" />
            <Limit name="measured-frame-rate-1920x1080" range="60-120" />
        </MediaCodec>
        <MediaCodec name="c2.android.vp8.encoder" type="video/x-vnd.on2.vp8" update="true">
            <Limit name="measured-frame-rate-320x180" range="634-805" />
            <Limit name="measured-frame-rate-640x360" range="189-293" />
            <Limit name="measured-frame-rate-1280x720" range="60-120" />
            <Limit name="measured-frame-rate-1920x1080" range="30-115" />
        </MediaCodec>
        <MediaCodec name="c2.android.vp9.encoder" type="video/x-vnd.on2.vp9" update="true">
            <Limit name="measured-frame-rate-320x180" range="618-786" />
            <Limit name="measured-frame-rate-640x360" range="122-262" />
            <Limit name="measured-frame-rate-1280x720" range="60-120" />
            <Limit name="measured-frame-rate-1920x1080" range="30-60" />
        </MediaCodec>
    </Encoders>
</MediaCodecs>
"""


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
    # Ensure DRM/KMS (apple-dcp card0 + dcpext card1..3 + drm-asahi renderD128) and V4L2 permissions
    mkdir /dev/dri 0755 system graphics
    chmod 0666 /dev/dri/card0
    chmod 0666 /dev/dri/card1
    chmod 0666 /dev/dri/card2
    chmod 0666 /dev/dri/card3
    chmod 0666 /dev/dri/renderD128
    chmod 0666 /dev/dri/renderD129
    chmod 0666 /dev/dri/renderD130
    chmod 0666 /dev/video0
    chmod 0666 /dev/video1
    chmod 0666 /dev/media0
    # Ensure 4K60 / 1080p60 MediaCodec & PerformancePoint XML variants are set before post-fs-data
    setprop vendor.media.target_variant _hamoa
    setprop ro.media.xml_variant.codecs _hamoa
    setprop ro.media.xml_variant.codecs_performance _hamoa

on init
    # Label Apple Silicon SoC GPU, DCP, ISP Camera, Backlight, and SMC sysfs nodes
    restorecon_recursive /sys/devices/platform/soc
    restorecon_recursive /sys/class/backlight
    restorecon_recursive /sys/class/power_supply
    restorecon_recursive /sys/class/drm
    restorecon_recursive /sys/class/video4linux
    chown system system /sys/class/backlight/apple-panel-bl/brightness
    chmod 0664 /sys/class/backlight/apple-panel-bl/brightness
    chown system system /sys/class/backlight/apple_backlight/brightness
    chmod 0664 /sys/class/backlight/apple_backlight/brightness
    setprop vendor.asahi.display.brightness 180
    start asahi-speakersafetyd
    start asahi-hw-daemon
    start asahi-workstation-daemon

on post-fs-data
    mkdir /data/asahi-linux 0755 root root
    mkdir /data/local/root-home 0700 root root
    mkdir /data/vendor/mediadrm 0770 media mediadrm
    # Prepare Wi-Fi HAL & wpa_supplicant runtime directories for Broadcom BCM4377 (brcmfmac wlan0)
    mkdir /dev/socket/wifihal 0770 wifi wifi
    chmod 2770 /dev/socket/wifihal
    mkdir /data/vendor/wifi 0770 wifi wifi
    mkdir /data/vendor/wifi/sockets 0770 wifi wifi
    mkdir /data/vendor/wifi/hostapd 0770 wifi wifi
    mkdir /data/vendor/wifi/hostapd/ctrl 0770 wifi wifi
    mkdir /data/vendor/wifi/wpa_supplicant 0770 wifi wifi
    mkdir /data/vendor/wifi/wpa 0770 wifi wifi
    mkdir /data/vendor/wifi/wpa/sockets 0770 wifi wifi
    setprop wifi.interface wlan0
    setprop wlan.driver.status ok
    setprop persist.drm.forcel3.enabled true

on property:vendor.asahi.bt.ready=1
    start vendor.bluetooth-aidl-qti

# Real-time TAS2764 V/I-sense thermal model + 125 Hz HPF + j313 EQ + lookahead limiter
service asahi-speakersafetyd /vendor/bin/asahi-speakersafetyd
    class core
    user root
    group root audio system
    seclabel u:r:init:s0

# Apple Silicon hardware bridge: BCM4377 BT HCI PTY, apple-panel-bl, macsmc-battery, lid/jack, dcpext
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

    # 5. Patch stock Wi-Fi HAL (libwifi-hal.so) for Broadcom brcmfmac (wlan0),
    #    Lights HAL (android.hardware.lights-service.android-desktop) for apple-panel-bl,
    #    and Camera HAL (android.hardware.camera.provider-usb-service.android-desktop)
    #    for Apple Silicon's built-in FaceTime HD Camera (apple-isp /dev/video0).
    add(
        "lib64/libwifi-hal.so",
        patch_wifi_hal_for_brcmfmac(original("lib64/libwifi-hal.so")),
        "vendor_file",
    )
    add(
        "bin/hw/android.hardware.lights-service.android-desktop",
        patch_lights_hal_for_apple_backlight(
            original("bin/hw/android.hardware.lights-service.android-desktop")
        ),
        "hal_light_default_exec",
        0o755,
    )
    add(
        "bin/hw/android.hardware.camera.provider-usb-service.android-desktop",
        patch_camera_hal_for_apple_isp(
            original("bin/hw/android.hardware.camera.provider-usb-service.android-desktop")
        ),
        "hal_camera_default_exec",
        0o755,
    )
    add(
        "etc/camera/internal_usb_camera_info.txtpb",
        original("etc/camera/internal_usb_camera_info.txtpb") + APPLE_ISP_CAMERA_INFO_TXTPB,
        "vendor_configs_file",
    )

    # 6. Install 4K60 / 1080p240 Codec2 Video & PerformancePoint configurations
    #    (removes the 720p cap on AV1/VP9/AVC/HEVC and disables dead Qualcomm /dev/video32 entries)
    add(
        "etc/media_codecs_google_c2_video.xml",
        ASAHI_MEDIA_CODECS_GOOGLE_C2_VIDEO_XML,
        "vendor_configs_file",
    )
    for mc_name in [
        "etc/media_codecs.xml",
        "etc/media_codecs_hamoa.xml",
        "etc/media_codecs_hamoa_vendor.xml",
    ]:
        add(mc_name, ASAHI_MEDIA_CODECS_HAMOA_XML, "vendor_configs_file")
    for mp_name in [
        "etc/media_codecs_performance_hamoa.xml",
        "etc/media_codecs_performance_hamoa_vendor.xml",
    ]:
        add(mp_name, ASAHI_MEDIA_CODECS_PERFORMANCE_XML, "vendor_configs_file")

    # 7. Remove QEMU-only guest helpers (including vm-offline-desktop) so bare-metal
    #    Googlebook OS runs its normal Setup Wizard, Lockscreen, and Screen Timeout.
    for vm_only in [
        "etc/init/vm-offline-desktop.rc",
        "bin/vm-offline-desktop.sh",
        "etc/init/vm-host-control.rc",
        "bin/vm-host-control.sh",
        "etc/init/vm-input.rc",
        "bin/vm-input-service.sh",
        "etc/init/vm-diagnostic-log.rc",
    ]:
        files.pop(vm_only, None)

    # 8. Update Bluetooth HAL service to accept /dev/asahi_bt_hci (bare-metal BCM4377)
    #    and strip QEMU-only `console ttyAMA0` from any remaining init .rc services.
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

    # 9. Switch vendor build.prop to Asahi AGX GLES + Honeykrisp Vulkan + 4K media + external display
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
        b"ro.surface_flinger.max_virtual_display_dimension=4096\n"
        b"vendor.hwc.drm.handle_all_uevents=1\n"
        b"vendor.wv.oemcrypto.debug.enable_external_display_check=false\n"
        b"persist.drm.forcel3.enabled=true\n"
        b"drm.service.enabled=true\n"
        b"vendor.media.target_variant=_hamoa\n"
        b"ro.media.xml_variant.codecs=_hamoa\n"
        b"ro.media.xml_variant.codecs_performance=_hamoa\n"
    )
    add("build.prop", b_prop, b_lbl, b_mode)

    # 10. Label Apple Silicon sysfs & device nodes in vendor_file_contexts and ueventd.rc
    ctx, ctx_lbl, ctx_mode = files["etc/selinux/vendor_file_contexts"]
    ctx += (
        b"\n# Bare-metal Apple Silicon (t8103) GPU, DCP, ISP Camera, Backlight, SMC & BT PTY contexts\n"
        b"/sys/devices/platform/soc(/.*)? u:object_r:sysfs_gpu:s0\n"
        b"/sys/class/drm(/.*)? u:object_r:sysfs_gpu:s0\n"
        b"/sys/class/video4linux(/.*)? u:object_r:sysfs_gpu:s0\n"
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
        b"\n# Bare-metal Apple Silicon DRM (dcp + dcpext), V4L2 ISP Camera, ALSA, Backlight & Input\n"
        b"/dev/dri/card0            0666   system     graphics\n"
        b"/dev/dri/card1            0666   system     graphics\n"
        b"/dev/dri/card2            0666   system     graphics\n"
        b"/dev/dri/card3            0666   system     graphics\n"
        b"/dev/dri/renderD128       0666   system     graphics\n"
        b"/dev/dri/renderD129       0666   system     graphics\n"
        b"/dev/dri/renderD130       0666   system     graphics\n"
        b"/dev/video*               0660   camera     camera\n"
        b"/dev/media*               0660   camera     camera\n"
        b"/dev/asahi_bt_hci         0660   bluetooth  bluetooth\n"
        b"/dev/snd/*                0660   system     audio\n"
        b"/dev/input/*              0660   root       input\n"
    )
    add("etc/ueventd.rc", ueventd_orig, "vendor_configs_file")

    return ["etc/camera", "usr", "usr/idc"], ["usr/keylayout"]
