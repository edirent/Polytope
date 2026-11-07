#!/usr/bin/env python3
"""Allowlisted source snapshot, secret exclusion, and content-addressed release manifest."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TOP = {'engine', 'oracle', 'replay', 'apps', 'frontend', 'config', 'docs', 'tools', 'tests', 'research'}
SUFFIXES = {'.cpp','.h','.hpp','.c','.cmake','.py','.json','.yaml','.yml','.md','.txt','.ts','.css','.html','.bin','.toml','.ini','.jsonl','.raw'}
EXCLUDE = {'node_modules','__pycache__','dist','runs','logs','.git','.local','secp256k1'}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def collect():
    files = []
    for top in sorted(TOP):
        for p in sorted((ROOT/top).rglob('*')):
            rel = p.relative_to(ROOT)
            if not p.is_file() or p.is_symlink() or any(x in EXCLUDE for x in rel.parts): continue
            if p.suffix not in SUFFIXES and p.name != 'CMakeLists.txt': continue
            if '.env' in p.name or p.suffix in {'.pem','.key','.pyc'} or 'credentials.local' in p.name: continue
            if top == 'research' and p.suffix not in {'.py','.md','.txt'}: continue
            if p.stat().st_size > 8*1024*1024: raise RuntimeError('source file requires explicit large-file review: '+str(rel))
            raw=p.read_bytes()
            if re.search(rb'^-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----\r?$',raw,re.M):
                raise RuntimeError('private-key material rejected at path '+str(rel))
            if p.suffix=='.json':
                try: obj=json.loads(raw)
                except (ValueError,UnicodeDecodeError): obj=None
                def inspect(v):
                    if isinstance(v,dict):
                        for k,x in v.items():
                            if k.lower() in {'api_key','api_key_id','private_key','private_key_pem','secret','signature','passphrase'} and x and not str(x).startswith(('${','YOUR_','<')):
                                raise RuntimeError('credential-like configuration rejected at path '+str(rel))
                            inspect(x)
                    elif isinstance(v,list):
                        for x in v: inspect(x)
                inspect(obj)
            files.append(p)
    files += [ROOT/'CMakeLists.txt',ROOT/'README.md',ROOT/'.gitignore']
    return sorted(set(files))

def command(args):
    p=subprocess.run(args,cwd=ROOT,capture_output=True,text=True,check=True)
    return p.stdout.strip()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--out',required=True);parser.add_argument('--binary')
    args=parser.parse_args(); out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=False)
    entries=[]
    for p in collect():
        rel=p.relative_to(ROOT);target=out/'source'/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
        entries.append({'path':str(rel),'sha256':digest(p),'bytes':p.stat().st_size})
    tree=hashlib.sha256(''.join(e['path']+'\0'+e['sha256']+'\n' for e in entries).encode()).hexdigest()
    manifest={'schema_version':1,'source_tree_sha256':tree,'files':entries,'git_head':command(['git','rev-parse','HEAD']),
              'compiler':command(['c++','--version']),'cmake':command(['cmake','--version']),
              'openssl':command(['openssl','version']),'dependencies':command(['dpkg-query','-W','-f=${Package} ${Version}\n','libboost-dev','libssl-dev','libstdc++6','libc6']),
              'build_profile':{'generator':'Unix Makefiles','CMAKE_BUILD_TYPE':'Release','ENABLE_LTO':'OFF','ENABLE_NATIVE_ARCH':'OFF','SOURCE_DATE_EPOCH':'1793923200'},
              'exclusions':sorted(EXCLUDE)+['.env*','credentials.local*','*.pem','*.key','research non-source data'],
              'sensitive_scan':'PASS_FOR_ALLOWLISTED_FILES','binary_sha256':digest(Path(args.binary)) if args.binary else None}
    (out/'release_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('Frozen source identity:',tree,'files:',len(entries))
if __name__=='__main__':main()
