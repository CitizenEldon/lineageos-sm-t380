#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Package the release locally. This tool never publishes anything."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
from release_bundle import ROOT, require, sha

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'dist')
    args=parser.parse_args()
    subprocess.run([sys.executable,'-B',str(ROOT/'tools/verify.py')],check=True)
    inventory=json.loads((ROOT/'repository-files.json').read_text())
    assets=json.loads((ROOT/'release/assets.json').read_text())
    require(set(assets)=={'installed-0.01.tar.gz','rescue-recovery.img.gz','kernel-source-0.01.tar.gz'},'Unexpected binary asset set')
    names=sorted(list(inventory)+['repository-files.json']+['release/'+n for n in assets])
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    path=out/'lineageos-sm-t380-0.01.zip'
    temporary=path.with_suffix('.zip.tmp')
    with zipfile.ZipFile(temporary,'w',allowZip64=True) as z:
        for name in names:
            require(not Path(name).is_absolute() and '..' not in Path(name).parts,'Unsafe inventory path')
            info=zipfile.ZipInfo('lineageos-sm-t380/'+name,date_time=(2026,10,13,15,48,0))
            info.create_system=3
            mode=0o755 if (ROOT/name).stat().st_mode & 0o111 else 0o644
            info.external_attr=(0o100000|mode)<<16
            info.compress_type=zipfile.ZIP_STORED if name.endswith('.gz') else zipfile.ZIP_DEFLATED
            with (ROOT/name).open('rb') as source,z.open(info,'w',force_zip64=True) as target:
                shutil.copyfileobj(source,target)
    temporary.replace(path)
    shutil.copyfile(ROOT/'release/kernel-source-0.01.tar.gz',out/'kernel-source-0.01.tar.gz')
    checksums=''.join(f'{sha(out/name)}  {name}\n' for name in [path.name,'kernel-source-0.01.tar.gz'])
    (out/'SHA256SUMS').write_text(checksums)
    print(json.dumps({'status':'LOCAL_RELEASE_PREPARED','uploadPerformed':False,'assets':[p.name for p in sorted(out.iterdir()) if p.is_file()]},indent=2))

if __name__=='__main__':main()
