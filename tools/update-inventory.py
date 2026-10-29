#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Refresh checksums for the deliberately public repository and its fixed assets."""
import json
from pathlib import Path
import subprocess
from release_bundle import ROOT, require, sha

ASSETS = ('installed-0.01.tar.gz','rescue-recovery.img.gz','kernel-source-0.01.tar.gz')

def main():
    assets={name:{'bytes':(ROOT/'release'/name).stat().st_size,'sha256':sha(ROOT/'release'/name)} for name in ASSETS}
    (ROOT/'release/assets.json').write_text(json.dumps(assets,indent=2,sort_keys=True)+'\n')
    result=subprocess.run(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=ROOT,check=True,capture_output=True)
    names=sorted(set(result.stdout.decode().strip('\0').split('\0'))-{'repository-files.json',''})
    inventory={}
    for name in names:
        path=ROOT/name
        require(name.split('/')[0] in {'README.md','LICENSE','.gitignore','.gitattributes','.github','docs','tools','source','trust','licenses','release'}, 'Unreviewed top-level public file: '+name)
        require(path.is_file() and not path.is_symlink(), 'Only regular repository files are accepted: '+name)
        require(not name.endswith(('.backup','.key','.p12','.pfx','.jks','.keystore')) and 'signing/' not in name and '.env' not in Path(name).parts, 'Private material must not enter the repository: '+name)
        require(not name.endswith('.pem') or name=='trust/distribution.public.pem', 'Unexpected key file: '+name)
        require(path.stat().st_size < 50*1024*1024, 'Large assets belong outside Git: '+name)
        inventory[name]={'bytes':path.stat().st_size,'sha256':sha(path)}
    (ROOT/'repository-files.json').write_text(json.dumps(inventory,indent=2,sort_keys=True)+'\n')
    print(json.dumps({'repositoryFiles':len(inventory),'releaseAssets':len(assets)}))

if __name__=='__main__':main()
