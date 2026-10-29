#!/usr/bin/env python3
"""Reconstruct retained inputs and custom sources; builds run on Linux x86_64."""
from pathlib import Path
import argparse
import bz2
import datetime
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'source'


def run(args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fingerprint(root):
    entries = {}
    for parent, directories, files in os.walk(root, followlinks=False):
        directories[:] = [name for name in directories if name != '.git']
        for name in files + [name for name in directories if (Path(parent) / name).is_symlink()]:
            path = Path(parent) / name
            relative = path.relative_to(root).as_posix()
            entries[relative] = ({'link': os.readlink(path)} if path.is_symlink() else
                                 {'sha256': digest(path), 'executable': bool(path.stat().st_mode & 0o111)})
    encoded = json.dumps(entries, sort_keys=True, separators=(',', ':')).encode()
    return {'files': len(entries), 'sha256': hashlib.sha256(encoded).hexdigest()}


def preserve_link(member, destination):
    safe = tarfile.data_filter(member, destination)
    if safe is not None and member.issym():
        safe.linkname = member.linkname
    return safe


def unpack_release(work):
    from release_bundle import verify_bundle
    destination = work / 'release'
    if destination.exists():
        # Validate existing outputs against the signed archive without overwriting.
        result = verify_bundle(ROOT / 'release/installed-0.01.tar.gz')
        for name, entry in result['images'].items():
            target = destination / name
            if target.stat().st_size != entry['bytes'] or digest(target) != entry['sha256']:
                raise ValueError('Existing unpacked image differs: '+name)
        return destination
    verify_bundle(ROOT / 'release/installed-0.01.tar.gz', destination)
    return destination


def bspatch(original, patch):
    """Apply standard BSDIFF40 without another retained executable."""
    def integer(raw):
        value = int.from_bytes(raw, 'little')
        return -(value & ((1 << 63) - 1)) if value >> 63 else value
    assert patch[:8] == b'BSDIFF40'
    control_size, diff_size, new_size = [integer(patch[n:n + 8]) for n in (8, 16, 24)]
    assert min(control_size, diff_size, new_size) >= 0
    control = bz2.decompress(patch[32:32 + control_size])
    differences = bz2.decompress(patch[32 + control_size:32 + control_size + diff_size])
    extra = bz2.decompress(patch[32 + control_size + diff_size:])
    result = bytearray(new_size)
    old_at = new_at = diff_at = extra_at = control_at = 0
    while new_at < new_size:
        assert control_at + 24 <= len(control)
        x, y, z = [integer(control[control_at + n:control_at + n + 8]) for n in (0, 8, 16)]
        control_at += 24
        assert x >= 0 and y >= 0 and new_at + x + y <= new_size
        assert diff_at + x <= len(differences) and extra_at + y <= len(extra)
        for offset in range(0, x, 1024 * 1024):
            size = min(x - offset, 1024 * 1024)
            changes = differences[diff_at + offset:diff_at + offset + size]
            start = old_at + offset
            if 0 <= start and start + size <= len(original):
                old = original[start:start + size]
                block = bytes((a + b) & 255 for a, b in zip(old, changes)) if any(changes) else old
            else:
                block = bytes(((original[start + i] if 0 <= start + i < len(original) else 0) + b) & 255
                              for i, b in enumerate(changes))
            result[new_at + offset:new_at + offset + size] = block
        new_at += x
        old_at += x + z
        diff_at += x
        result[new_at:new_at + y] = extra[extra_at:extra_at + y]
        new_at += y
        extra_at += y
    assert diff_at == len(differences) and extra_at == len(extra)
    return result


def restore_inputs(work):
    release = unpack_release(work)
    config = json.loads((SOURCE / 'inputs.json').read_text())
    android = work / 'android'
    boot = (release / 'boot.img').read_bytes()
    assert boot[:8] == b'ANDROID!'
    kernel, _, ramdisk, _, second, _, _, page, dt, _ = struct.unpack_from('<10I', boot, 8)
    align = lambda n: (n + page - 1) // page * page
    table_at = page + align(kernel) + align(ramdisk) + align(second)
    for entry in config['binaryInputs']:
        destination = android / entry['path']
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() and digest(destination) == entry['sha256']:
            continue
        if entry.get('fromBoot') == 'kernel':
            destination.write_bytes(boot[page:page + kernel])
        elif entry.get('fromBoot') == 'qcdt':
            destination.write_bytes(boot[table_at:table_at + dt])
        else:
            # debugfs writes only the requested host output file; the image is opened read-only.
            temporary = work / 'input-from-image.tmp'
            assert '"' not in str(temporary) and '"' not in entry['releasePath']
            run(['debugfs', '-R', f'dump "{entry["releasePath"]}" "{temporary}"', release / 'system.img'],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            assert temporary.exists(), entry['path']
            if entry.get('delta'):
                assert digest(temporary) == entry['releaseSha256']
                destination.write_bytes(bspatch(temporary.read_bytes(), (SOURCE / entry['delta']).read_bytes()))
                temporary.unlink()
            else:
                temporary.replace(destination)
        assert destination.stat().st_size == entry['bytes'] and digest(destination) == entry['sha256'], entry['path']


def restore_source(work):
    android = work / 'android'
    assert (android / '.repo').is_dir(), 'Sync source/android.xml with repo first; see README.'
    config = json.loads((SOURCE / 'inputs.json').read_text())
    for destination in [work / 'kernel-build', android / 'kernel/samsung/gta2swifi']:
        assert not destination.exists(), f'Refusing to overwrite {destination}'
        destination.mkdir(parents=True)
        run(['git', '-C', destination, 'init', '-q'])
        run(['git', '-C', destination, 'fetch', '--depth=1', config['kernel']['url'], config['kernel']['base']])
        run(['git', '-C', destination, 'checkout', '--detach', 'FETCH_HEAD'])
    for entry in config['patches']:
        destination = work / 'kernel-build' if entry['path'] == 'kernel-build' else android / entry['path']
        actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
        assert actual == entry['base'], entry['path']
        patch = SOURCE / entry['patch']
        assert digest(patch) == entry['sha256']
        run(['git', '-C', destination, 'apply', '--check', patch])
        run(['git', '-C', destination, 'apply', patch])
    with tarfile.open(SOURCE / 'overlay.tar.gz') as archive:
        for member in archive:
            destination = work if member.name.startswith('kernel-build/') else android
            archive.extract(member, destination, filter=preserve_link)
    restore_inputs(work)
    assert fingerprint(work / 'kernel-build') == config['fingerprints']['kernel']
    assert fingerprint(android / 'kernel/samsung/gta2swifi') == config['fingerprints']['uapi']
    print('Sources and all binary inputs reconstructed and verified.')


def build_kernel(work, jobs):
    config = json.loads((SOURCE / 'inputs.json').read_text())
    output = work / 'kernel-out'
    output.mkdir(exist_ok=False)
    shutil.copy2(SOURCE / 'kernel.config', output / '.config')
    clang = work / 'android' / config['kernel']['toolchainInAndroid'] / 'bin'
    epoch = config['kernel']['sourceEpoch']
    timestamp = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).strftime('%a %b %d %H:%M:%S UTC %Y')
    env = {**os.environ, 'PATH': str(clang) + ':' + os.environ['PATH'], 'ARCH': 'arm64', 'LC_ALL': 'C',
           'TZ': 'UTC', 'KBUILD_BUILD_USER': 't380', 'KBUILD_BUILD_HOST': 't380-build',
           'KBUILD_BUILD_VERSION': '1', 'SOURCE_DATE_EPOCH': str(epoch), 'KBUILD_BUILD_TIMESTAMP': timestamp}
    command = ['make', '-C', work / 'kernel-build', 'O=' + str(output), 'ARCH=arm64', 'LLVM=1', 'LLVM_IAS=1']
    run(command + ['olddefconfig'], env=env)
    run(command + ['CROSS_COMPILE=aarch64-linux-gnu-', 'CROSS_COMPILE_COMPAT=arm-linux-gnueabi-',
                   '-j' + str(jobs), 'Image.gz', 'dtbs', 'modules'], env=env)
    print('Kernel built in', output, '; review and stage its kernel/DT/module together before an Android release.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=['unpack', 'source', 'inputs', 'kernel'])
    parser.add_argument('workspace', type=Path)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    work = args.workspace.resolve()
    work.mkdir(parents=True, exist_ok=True)
    {'unpack': unpack_release, 'source': restore_source, 'inputs': restore_inputs,
     'kernel': lambda w: build_kernel(w, args.jobs)}[args.operation](work)


if __name__ == '__main__':
    main()
