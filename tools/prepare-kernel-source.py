#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Assemble the complete exact kernel source on macOS or Linux, without compiling."""
import argparse
import gzip
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
from release_bundle import ROOT, require, sha

UPSTREAM_URL = 'https://codeload.github.com/LineageOS/android_kernel_xiaomi_msm8937/tar.gz/704f6f08135c39728fcbe6f94b3956d911bb1801'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('workspace',type=Path,help='New working directory')
    parser.add_argument('--upstream-archive',type=Path)
    parser.add_argument('--output',type=Path,default=ROOT/'release/kernel-source-0.01.tar.gz')
    args=parser.parse_args()
    work=args.workspace.resolve()
    work.mkdir(parents=True,exist_ok=False)
    with tempfile.TemporaryDirectory(dir=work) as test:
        probe=Path(test)/'t380Case'
        probe.write_text('case-sensitive filesystem check')
        require(not (Path(test)/'T380CASE').exists(), 'Kernel source requires a case-sensitive filesystem. On macOS use a case-sensitive APFS disk image; see docs/BUILDING.md.')
    require(not args.output.exists(), 'Output already exists')
    config=json.loads((ROOT/'source/inputs.json').read_text())
    spec=importlib.util.spec_from_file_location('develop', ROOT/'tools/develop.py')
    develop=importlib.util.module_from_spec(spec);spec.loader.exec_module(develop)
    source_archive=args.upstream_archive
    if source_archive is None:
        source_archive=work/'upstream.tar.gz'
        with urllib.request.urlopen(UPSTREAM_URL,timeout=120) as r,source_archive.open('wb') as f:
            shutil.copyfileobj(r,f)
    package=work/'kernel-source-0.01'
    package.mkdir()
    kernel=package/'kernel'
    kernel.mkdir()
    with tarfile.open(source_archive) as t:
        for member in t:
            parts=Path(member.name).parts
            if len(parts)<2:
                continue
            member.name='/'.join(parts[1:])
            t.extract(member,kernel,filter=develop.preserve_link)
    patch=ROOT/'source/patches/kernel-build.patch'
    expected=next(p for p in config['patches'] if p['path']=='kernel-build')
    require(sha(patch)==expected['sha256'],'Kernel patch hash differs')
    subprocess.run(['git','apply','--check',str(patch)],cwd=kernel,check=True)
    subprocess.run(['git','apply',str(patch)],cwd=kernel,check=True)
    with tarfile.open(ROOT/'source/overlay.tar.gz') as t:
        for member in t:
            if member.name.startswith('kernel-build/'):
                member.name=member.name[len('kernel-build/'):]
                t.extract(member,kernel,filter=develop.preserve_link)
    fingerprint=develop.fingerprint(kernel)
    require(fingerprint==config['fingerprints']['kernel'],'Reconstructed kernel fingerprint differs: '+str(fingerprint))
    shutil.copyfile(ROOT/'source/kernel.config',package/'kernel.config')
    proof={'status':'PASS','sourceFingerprint':fingerprint,'upstreamUrl':UPSTREAM_URL,
           'baseRevision':config['kernel']['base'],'configurationSha256':sha(package/'kernel.config'),
           'sourceEpoch':config['kernel']['sourceEpoch'],'toolchainInAndroid':config['kernel']['toolchainInAndroid'],
           'sourceCompilationPerformed':False}
    (package/'source-receipt.json').write_text(json.dumps(proof,indent=2)+'\n')
    (package/'BUILDING.txt').write_text('Complete kernel source for version 0.01, verified against the recorded source fingerprint.\nCopy kernel.config to your kernel output .config. Use the pinned Linux x86-64 Clang toolchain and ARM cross-compilers specified in the release source/android.xml and source/inputs.json. See docs/BUILDING.md and tools/develop.py for the exact build commands and source epoch. Kernel, device tree and WLAN module must be staged together.\n')
    def normalize(member):
        member.uid=member.gid=0;member.uname=member.gname='';member.mtime=config['kernel']['sourceEpoch'];member.pax_headers={}
        return member
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('xb') as f,gzip.GzipFile(filename='',mode='wb',fileobj=f,mtime=0) as gz,tarfile.open(fileobj=gz,mode='w') as t:
        t.add(package,arcname='kernel-source-0.01',filter=normalize)
    print(json.dumps({**proof,'archiveSha256':sha(args.output),'archiveBytes':args.output.stat().st_size},indent=2))

if __name__=='__main__':
    main()
