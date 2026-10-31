#!/usr/bin/env python3
"""Pinned official model/runtime retrieval; all writes confined to this backend.
No login, installer, build toolchain download, global config or deployment.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
import urllib.request
import zipfile
ROOT=Path(__file__).resolve().parents[1]
MANIFEST=json.loads((ROOT/'provenance.json').read_text())

def sha(data):return hashlib.sha256(data).hexdigest()
def atomic(path,data):
 path.parent.mkdir(parents=True,exist_ok=True)
 with tempfile.NamedTemporaryFile(dir=path.parent,delete=False) as f:
  tmp=Path(f.name);f.write(data)
 try:
  tmp.chmod(0o644);os.replace(tmp,path)
 finally:
  tmp.unlink(missing_ok=True)

def main():
 p=argparse.ArgumentParser();p.add_argument('--host',action='store_true',help='also obtain pinned macOS arm64 CPython 3.11 test wheels')
 p.add_argument('--verify-only',action='store_true');a=p.parse_args()
 for item in MANIFEST['assets']:
  if item.get('host_test_only') and not a.host:continue
  out=ROOT/item['destination']
  if out.is_file() and sha(out.read_bytes())==item['sha256']:continue
  if a.verify_only:raise RuntimeError('missing/mismatched '+str(out))
  with urllib.request.urlopen(item['url'],timeout=60) as response:data=response.read(64*1024*1024)
  if sha(data)!=item['sha256']:raise RuntimeError('SHA256 mismatch: '+item['destination'])
  atomic(out,data)
 archive=ROOT/'downloads/litert-1.2.0.aar'
 with zipfile.ZipFile(archive) as z:
  entries={'jni/arm64-v8a/libtensorflowlite_jni.so':('runtime/android-arm64/libtensorflowlite_jni.so',MANIFEST['runtime_sha256']),
           'LICENSE':('runtime/LICENSE.tensorflow',sha(z.read('LICENSE')))}
  entries.update({'headers/'+name:('runtime/include/'+name,digest) for name,digest in MANIFEST['installed_headers'].items()})
  for entry,(dest,digest) in entries.items():
   data=z.read(entry)
   if sha(data)!=digest:raise RuntimeError('archive member mismatch '+entry)
   path=ROOT/dest
   if not path.is_file() or sha(path.read_bytes())!=digest:
    if a.verify_only:raise RuntimeError('missing/mismatched installed '+dest)
    atomic(path,data)
 if a.host and not a.verify_only:
  wheels=list((ROOT/'downloads').glob('ai_edge_litert-1.2.0-cp311-*.whl'))
  with zipfile.ZipFile(wheels[0]) as z:
   for entry in z.namelist():
    rel=Path(entry)
    if entry.endswith('/'):continue
    if rel.is_absolute() or '..' in rel.parts:raise RuntimeError('unsafe wheel path')
    atomic(ROOT/'runtime/host-wheel'/rel,z.read(entry))
 print(json.dumps({'verified':True,'models':2,'runtime':'Google LiteRT 1.2.0','runtime_sha256':MANIFEST['runtime_sha256'],'host_assets':a.host}))
if __name__=='__main__':main()
