#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import argparse
import gzip
import json
import hashlib
import os
import tarfile
from pathlib import Path
from release_bundle import ROOT, read_json, require, sha, verify_bundle

def main():
    parser = argparse.ArgumentParser(description='Verify repository files and optional local release assets; never contacts a device.')
    parser.add_argument('--repository-only', action='store_true')
    args = parser.parse_args()
    inventory = read_json((ROOT/'repository-files.json').read_text())
    for name, entry in inventory.items():
        path = ROOT/name
        require(path.is_file() and not path.is_symlink(), 'Missing/nonregular repository file: '+name)
        require(path.stat().st_size == entry['bytes'] and sha(path) == entry['sha256'], 'Repository file mismatch: '+name)
    overlay_names=set()
    with tarfile.open(ROOT/'source/overlay.tar.gz') as overlay:
        for member in overlay:
            require(member.name not in overlay_names, 'Duplicate overlay entry')
            overlay_names.add(member.name)
            path=ROOT/'source/overlay'/member.name
            if member.isfile():
                require(path.is_file() and not path.is_symlink() and hashlib.file_digest(overlay.extractfile(member),'sha256').hexdigest()==sha(path), 'Readable overlay differs: '+member.name)
            elif member.issym():
                require(path.is_symlink() and os.readlink(path)==member.linkname, 'Overlay symlink differs')
            else:
                raise ValueError('Unexpected overlay member type')
    readable={p.relative_to(ROOT/'source/overlay').as_posix() for p in (ROOT/'source/overlay').rglob('*') if p.is_file() or p.is_symlink()}
    require(readable==overlay_names, 'Readable overlay has missing or extra files')
    report = {'repositoryFilesVerified': len(inventory), 'overlayEntriesVerified':len(overlay_names),'deviceActions': False}
    if not args.repository_only:
        assets = read_json((ROOT/'release/assets.json').read_text())
        for name, entry in assets.items():
            path = ROOT/'release'/name
            require(path.is_file() and not path.is_symlink(), 'Missing release asset: '+name)
            require(path.stat().st_size == entry['bytes'] and sha(path) == entry['sha256'], 'Release asset mismatch: '+name)
        with gzip.open(ROOT/'release/rescue-recovery.img.gz','rb') as stream:
            data=stream.read(33554433)
            require(len(data) == 33554432, 'Unexpected raw recovery length')
            require(hashlib.sha256(data).hexdigest() == 'b1fa28acd454d145e0f650b216c36e3f361cd91988e97f46f65ea4fe923147fb', 'Recovery content mismatch')
        report['release'] = verify_bundle(ROOT/'release/installed-0.01.tar.gz')
        report['releaseAssetsVerified'] = len(assets)
    print(json.dumps({'status':'PASS', **report}, indent=2))

if __name__ == '__main__':
    main()
