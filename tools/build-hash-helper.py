#!/usr/bin/env python3
"""Isolated public-source SHA256 helper build; no device or Android build writes."""
import argparse,hashlib,json,os,shutil,subprocess,time
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('android',type=Path)
parser.add_argument('output',type=Path)
args=parser.parse_args()
SOURCE=args.android.resolve()/'external/toybox'
ROOT=args.output.resolve()
assert subprocess.check_output(['git','-C',str(SOURCE),'rev-parse','HEAD'],text=True).strip()=='6209e96a73014f40e476cfb64cc351f32382a075'
assert not subprocess.check_output(['git','-C',str(SOURCE),'status','--porcelain'],text=True).strip()
DEST=ROOT/'source'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def inventory(root):
    result={}
    for p in sorted(root.rglob('*')):
        if '.git' in p.relative_to(root).parts:continue
        if p.is_symlink():result[str(p.relative_to(root))]={'symlink':os.readlink(p)}
        elif p.is_file():result[str(p.relative_to(root))]={'bytes':p.stat().st_size,'sha256':sha(p)}
    return result
ROOT.mkdir(parents=True,exist_ok=False)
assert not DEST.exists(),'build source already exists; inspect instead of duplicate'
before=inventory(SOURCE)
shutil.copytree(SOURCE,DEST,symlinks=True,ignore=shutil.ignore_patterns('.git'))
assert inventory(DEST)==before
(ROOT/'source-manifest.json').write_text(json.dumps(before,indent=2,sort_keys=True)+'\n')
env={**os.environ,'CROSS_COMPILE':'/usr/bin/arm-linux-gnueabihf-','CC':'gcc','CPUS':'2','LDFLAGS':'-static','LC_ALL':'C','LANG':'C'}
commands=[['make','allnoconfig'],['bash','scripts/single.sh','sha256sum']]
for i,cmd in enumerate(commands):
    with (ROOT/f'build-{i}.log').open('wb') as log:
        r=subprocess.run(cmd,cwd=DEST,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=180)
    assert r.returncode==0,f'build command failed {cmd}'
binary=ROOT/'sha256sum';shutil.copyfile(DEST/'sha256sum',binary);binary.chmod(0o755)
elf=subprocess.check_output(['/usr/bin/arm-linux-gnueabihf-readelf','-h','-l','-d',str(binary)],text=True)
(ROOT/'elf.txt').write_text(elf)
assert 'Machine:                           ARM' in elf and 'ELF32' in elf and 'INTERP' not in elf and 'There is no dynamic section' in elf
assert 'CONFIG_TOYBOX_LIBCRYPTO=y' not in (DEST/'.singleconfig').read_text()
(ROOT/'singleconfig').write_bytes((DEST/'.singleconfig').read_bytes())
assert inventory(SOURCE)==before,'live source changed while copied (not modified by this build)'
compiler=Path('/usr/bin/arm-linux-gnueabihf-gcc')
result={'schema':1,'status':'STATIC_ARM32_BUILD_PASS','source':str(SOURCE),'sourceGitCommit':subprocess.check_output(['git','-C',str(SOURCE),'rev-parse','HEAD'],text=True).strip(),'sourceManifestSha256':sha(ROOT/'source-manifest.json'),'compiler':{'path':str(compiler),'resolvedPath':str(compiler.resolve()),'sha256':sha(compiler),'version':subprocess.check_output([str(compiler),'--version'],text=True)},'libcStaticSha256':sha(Path('/usr/arm-linux-gnueabihf/lib/libc.a')),'configSha256':sha(ROOT/'singleconfig'),'binary':{'path':str(binary),'bytes':binary.stat().st_size,'sha256':sha(binary)},'commands':commands,'environment':{k:env[k] for k in ('CROSS_COMPILE','CC','CPUS','LDFLAGS')},'builtinSha256':True,'elfProofSha256':sha(ROOT/'elf.txt'),'qemu':shutil.which('qemu-arm') or shutil.which('qemu-arm-static'),'executedOnDevice':False,'deviceVectorsPending':True}
(ROOT/'build-receipt.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'binary':result['binary'],'originalVerifiedBinary':result['binary']['sha256']=='9d06af9f6389dc518a5d1eb67ac411c6d19d88a413e07b552a4a6b82b1082b01'}))
