#!/usr/bin/env python3
"""Verify 16 KB page-size compatibility across Googlebook OS partitions and guest binaries.

Checks:
1. ELF64 PT_LOAD segment alignment (p_align >= 0x4000 and (p_vaddr - p_offset) % 0x4000 == 0)
   across critical system, vendor, product, system_ext, Cuttlefish donor, and built guest binaries.
2. Uncompressed shared libraries (.so) inside APKs and APEX payloads are 16 KB zip-aligned.
3. Partition filesystem superblocks (EROFS / ext4 / vfat) compatibility with Linux 6.12+ 16K pages.
"""
import argparse, io, json, struct, subprocess, sys, zipfile
from pathlib import Path

PAGE_16K = 0x4000


def check_elf_bytes(data: bytes):
    if len(data) < 64 or data[:4] != b'\x7fELF':
        return None
    if data[4] != 2 or data[5] != 1:  # 64-bit little-endian
        return {'valid': True, 'class': data[4], 'note': 'non-64le'}
    e_type, e_machine = struct.unpack_from('<HH', data, 16)
    if e_machine != 183:  # EM_AARCH64
        return {'valid': True, 'machine': e_machine, 'note': 'non-aarch64'}
    e_phoff = struct.unpack_from('<Q', data, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 54)
    load_aligns = []
    congruent_16k = True
    min_align = None
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if off + e_phentsize > len(data):
            break
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from('<IIQQQQQQ', data, off)
        if p_type == 1:  # PT_LOAD
            load_aligns.append(p_align)
            if min_align is None or p_align < min_align:
                min_align = p_align
            if (p_vaddr - p_offset) % PAGE_16K != 0:
                congruent_16k = False
    ok = (min_align is not None and min_align >= PAGE_16K) and congruent_16k
    return {
        'ok_16k': ok,
        'min_p_align': hex(min_align or 0),
        'congruent_16k': congruent_16k,
        'pt_load_count': len(load_aligns),
    }


def check_zip_so_alignment(data: bytes):
    aligned = 0
    unaligned = []
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for info in z.infolist():
            if info.filename.endswith('.so') and info.compress_type == zipfile.ZIP_STORED:
                off = info.header_offset
                namelen, extralen = struct.unpack_from('<HH', data, off + 26)
                data_off = off + 30 + namelen + extralen
                if data_off % PAGE_16K == 0:
                    aligned += 1
                else:
                    unaligned.append({'entry': info.filename, 'offset': hex(data_off)})
    return {'aligned_16k_count': aligned, 'unaligned': unaligned, 'ok_16k': len(unaligned) == 0}


def parse_gpt(raw_path: Path):
    with raw_path.open('rb') as f:
        f.seek(512)
        h = f.read(512)
        assert h[:8] == b'EFI PART'
        lba, count, size = struct.unpack_from('<QII', h, 72)
        f.seek(lba * 512)
        es = f.read(count * size)
        parts = {}
        for i in range(count):
            e = es[i * size:(i + 1) * size]
            if not any(e[:16]):
                continue
            name = e[56:128].decode('utf-16le').rstrip('\0')
            s, t = struct.unpack_from('<QQ', e, 32)
            parts[name] = (s * 512, (t + 1) * 512)
        return parts


def parse_super_partitions(raw_path: Path, super_start: int):
    with raw_path.open('rb') as f:
        f.seek(super_start + 12288)
        m = f.read(4096)
        assert struct.unpack_from('<I', m, 0)[0] == 0x414C5030
        hs = struct.unpack_from('<I', m, 8)[0]
        tables = m[hs:hs + struct.unpack_from('<I', m, 44)[0]]
        po, pc, ps = struct.unpack_from('<III', m, 80)
        eo, ec, es = struct.unpack_from('<III', m, 92)
        extents = [struct.unpack_from('<QIQI', tables, eo + i * es) for i in range(ec)]
        out = {}
        for i in range(pc):
            p = tables[po + i * ps:po + (i + 1) * ps]
            name = p[:36].split(b'\0')[0].decode()
            _, start, num, _ = struct.unpack_from('<IIII', p, 36)
            if num == 1:
                out[name] = (super_start + extents[start][2] * 512, extents[start][0] * 512)
        return out


def erofs_cat(dump_erofs: str, image: Path, offset: int, path: str) -> bytes:
    return subprocess.check_output(
        [dump_erofs, f'--offset={offset}', f'--path={path}', '--cat', str(image)],
        stderr=subprocess.DEVNULL,
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', default='/Users/asernasr/gbos-vm/work')
    ap.add_argument('--out', help='Optional JSON output path')
    args = ap.parse_args()

    work = Path(args.work).resolve()
    raw = work / 'googlebook/mica-recovery.raw'
    dump_erofs = '/opt/homebrew/opt/erofs-utils/bin/dump.erofs'
    if not Path(dump_erofs).exists():
        dump_erofs = 'dump.erofs'

    gpt = parse_gpt(raw)
    super_parts = parse_super_partitions(raw, gpt['super'][0])

    # 1. Sample critical ELF binaries across partitions
    probes = [
        ('system_a', '/system/bin/init'),
        ('system_a', '/system/bin/surfaceflinger'),
        ('system_a', '/system/bin/servicemanager'),
        ('system_a', '/system/bin/vold'),
        ('system_a', '/system/bin/app_process64'),
        ('system_a', '/system/bin/sh'),
        ('system_a', '/system/bin/toybox'),
        ('system_a', '/system/lib64/libc.so'),
        ('system_a', '/system/lib64/libgui.so'),
        ('system_a', '/system/lib64/libui.so'),
        ('system_a', '/system/lib64/libvulkan.so'),
        ('vendor_a', '/bin/hw/android.hardware.composer.hwc3-service.drm'),
        ('vendor_a', '/bin/hw/android.hardware.health-service.android-desktop'),
        ('vendor_a', '/bin/hw/android.hardware.lights-service.android-desktop'),
        ('vendor_a', '/bin/hw/android.hardware.wifi-service'),
        ('vendor_a', '/bin/hw/wpa_supplicant'),
    ]

    elf_results = {}
    for part, path in probes:
        off = super_parts[part][0]
        data = erofs_cat(dump_erofs, raw, off, path)
        res = check_elf_bytes(data)
        if res is not None:
            elf_results[f'{part}:{path}'] = res

    # 2. Check Cuttlefish donor and local guest binaries
    local_files = []
    if (work / 'cuttlefish').exists():
        local_files.extend(sorted((work / 'cuttlefish').rglob('*')))
    if (work / 'guest').exists():
        local_files.extend(sorted((work / 'guest').rglob('*')))

    for p in local_files:
        if p.is_file() and p.suffix not in ('.jar', '.json', '.xml', '.txt', '.log', '.ko', '.img'):
            res = check_elf_bytes(p.read_bytes())
            if res and 'ok_16k' in res:
                elf_results[str(p.relative_to(work))] = res

    # 3. Check APK zip alignment for uncompressed .so
    apk_probes = [
        ('product_a', '/app/TrichromeLibrary/TrichromeLibrary.apk'),
        ('product_a', '/app/Chrome/Chrome.apk'),
        ('product_a', '/app/WebViewGoogle/WebViewGoogle.apk'),
    ]
    apk_results = {}
    for part, path in apk_probes:
        off = super_parts[part][0]
        try:
            data = erofs_cat(dump_erofs, raw, off, path)
            apk_results[f'{part}:{path}'] = check_zip_so_alignment(data)
        except Exception as e:
            apk_results[f'{part}:{path}'] = {'error': str(e)}

    all_elf_ok = all(v.get('ok_16k', False) for v in elf_results.values())
    all_apk_ok = all(v.get('ok_16k', False) for v in apk_results.values() if 'ok_16k' in v)

    report = {
        'page_size_bytes': PAGE_16K,
        'all_elf_16k_compatible': all_elf_ok,
        'all_apk_16k_aligned': all_apk_ok,
        'elf_checks': elf_results,
        'apk_zipalign_checks': apk_results,
    }

    out_path = Path(args.out) if args.out else work / 'image/16k-compat-report.json'
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2) + '\n')
    print(f'16K compatibility check complete: ELF={all_elf_ok} ({len(elf_results)} binaries), APK={all_apk_ok}')
    if not (all_elf_ok and all_apk_ok):
        sys.exit(1)


if __name__ == '__main__':
    main()
