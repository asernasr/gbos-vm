#!/usr/bin/env python3
"""Repack stock 4 KB ext4 partitions (metadata, persist, dsp_a) into 16 KB block-size ext4
partitions (-b 16384) for bare-metal Apple Silicon (CONFIG_ARM64_16K_PAGES=y) compatibility.

Preserves all files, directories, uid/gid/mode metadata, and security.selinux xattrs.
"""
import argparse, hashlib, json, re, struct, subprocess, tempfile
from pathlib import Path

EA_RE = re.compile(r'=\s*"([^"]*)"')


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


def get_selinux_ea(debugfs: str, img_path: Path, path: str) -> str:
    out = subprocess.run(
        [debugfs, '-R', f'ea_get {path} security.selinux', str(img_path)],
        capture_output=True, text=True
    ).stdout.strip()
    m = EA_RE.search(out)
    if not m:
        return ''
    val = m.group(1).replace('\\000', '')
    return val


def walk_ext4(debugfs: str, img_path: Path, cur: str = '/'):
    out = subprocess.check_output(
        [debugfs, '-R', f'ls -p {cur}', str(img_path)],
        text=True, stderr=subprocess.DEVNULL
    )
    entries = []
    for line in out.splitlines():
        if not line.startswith('/'):
            continue
        fields = line.split('/')
        if len(fields) < 7:
            continue
        ino, mode_str, uid, gid, name, size_str = fields[1:7]
        if name in ('.', '..', 'lost+found', ''):
            continue
        full = cur.rstrip('/') + '/' + name
        mode = int(mode_str, 8)
        ea = get_selinux_ea(debugfs, img_path, full)
        entries.append((full, mode, int(uid), int(gid), ea))
        if (mode & 0o170000) == 0o040000:
            entries.extend(walk_ext4(debugfs, img_path, full))
    return entries


def repack_partition(debugfs: str, mke2fs: str, e2fsck: str, src_img: Path, dst_img: Path):
    size = src_img.stat().st_size
    assert size % 16384 == 0, f'Partition size {size} is not a multiple of 16384'
    root_ea = get_selinux_ea(debugfs, src_img, '/')
    entries = walk_ext4(debugfs, src_img, '/')

    with tempfile.TemporaryDirectory() as tmp:
        stage = Path(tmp) / 'stage'
        stage.mkdir()
        for full, mode, uid, gid, ea in entries:
            rel = full.lstrip('/')
            target = stage / rel
            if (mode & 0o170000) == 0o040000:
                target.mkdir(parents=True, exist_ok=True)
            elif (mode & 0o170000) == 0o100000:
                target.parent.mkdir(parents=True, exist_ok=True)
                data = subprocess.check_output(
                    [debugfs, '-R', f'cat {full}', str(src_img)],
                    stderr=subprocess.DEVNULL
                )
                target.write_bytes(data)

        dst_img.write_bytes(b'\x00' * size)
        subprocess.run(
            [mke2fs, '-q', '-F', '-t', 'ext4', '-b', '16384', '-d', str(stage), str(dst_img)],
            check=True, stderr=subprocess.DEVNULL
        )

        # Apply exact uid, gid, mode, and security.selinux xattrs via debugfs -w
        cmds = []
        if root_ea:
            cmds.append(f'ea_set / security.selinux {root_ea}\\000')
        for full, mode, uid, gid, ea in entries:
            cmds.append(f'set_inode_field {full} mode 0{mode:o}')
            cmds.append(f'set_inode_field {full} uid {uid}')
            cmds.append(f'set_inode_field {full} gid {gid}')
            if ea:
                cmds.append(f'ea_set {full} security.selinux {ea}\\000')

        if cmds:
            cmd_file = Path(tmp) / 'debugfs.cmds'
            cmd_file.write_text('\n'.join(cmds) + '\n')
            subprocess.run(
                [debugfs, '-w', '-f', str(cmd_file), str(dst_img)],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )

    subprocess.run([e2fsck, '-fy', str(dst_img)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return {
        'size_bytes': size,
        'block_size': 16384,
        'entries_preserved': len(entries),
        'root_selinux': root_ea,
        'sha256': hashlib.sha256(dst_img.read_bytes()).hexdigest(),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('image', help='Path to cloned googlebook.raw or directory for standalone partition images')
    ap.add_argument('--source-raw', default='/Users/asernasr/gbos-vm/work/googlebook/mica-recovery.raw')
    ap.add_argument('--e2fsprogs-sbin', default='/opt/homebrew/opt/e2fsprogs/sbin')
    args = ap.parse_args()

    sbin = Path(args.e2fsprogs_sbin)
    debugfs = str(sbin / 'debugfs') if (sbin / 'debugfs').exists() else 'debugfs'
    mke2fs = str(sbin / 'mke2fs') if (sbin / 'mke2fs').exists() else 'mke2fs'
    e2fsck = str(sbin / 'e2fsck') if (sbin / 'e2fsck').exists() else 'e2fsck'

    src_raw = Path(args.source_raw).resolve()
    target = Path(args.image).resolve()
    gpt = parse_gpt(src_raw)

    report = {}
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        for name in ('metadata', 'persist', 'dsp_a'):
            s, e = gpt[name]
            src_part = tmp / f'{name}.4k.img'
            dst_part = tmp / f'{name}.16k.img'
            with src_raw.open('rb') as f:
                f.seek(s)
                src_part.write_bytes(f.read(e - s))
            info = repack_partition(debugfs, mke2fs, e2fsck, src_part, dst_part)
            report[name] = info
            if target.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                (target / f'{name}.16k.img').write_bytes(dst_part.read_bytes())
            else:
                assert target != src_raw, 'Refusing to modify original recovery image'
                with target.open('r+b') as f:
                    f.seek(s)
                    f.write(dst_part.read_bytes())

    out_json = (target if target.is_dir() else target.parent) / 'ext4-16k-repack.json'
    out_json.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
