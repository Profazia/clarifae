"""Synthetic mount-ID parser checks; real Android descriptor behavior is separate."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'live-targets.sh').read_text()
mid=s[s.index('mid(){'):s.index('atomic_text(){')]
assert 'while read -r _mid_key' in mid
assert "$(awk '$1==\"mnt_id:\"" not in mid
checks=0
with tempfile.TemporaryDirectory(prefix='routing-fdinfo-') as d:
 r=Path(d);target=r/'target.xml';target.write_text('<test/>\n')
 (r/'mountinfo').write_text(f'32 1 0:1 / {target} rw - tmpfs tmpfs rw\n')
 body=mid.replace('/proc/self/fdinfo/9',str(r/'fdinfo')).replace('/proc/1/mountinfo',str(r/'mountinfo'))
 script='set -eu\npositive_id(){ case "$1" in ""|0|*[!0-9]*)return 1;;esac; }\n'+body+'\nmid "$1"\n'
 cases=[('pos:\t0\nflags:\t0100000\nmnt_id:\t32\nino:\t1\n',True),('mnt_id: 32\nmnt_id: 32\n',False),('ino: 1\n',False),('mnt_id: nope\n',False),('mnt_id: 32 extra\n',False),('mnt_id: 99\n',False)]
 for data,ok in cases:
  (r/'fdinfo').write_text(data)
  p=subprocess.run(['sh','-c',script,'test',str(target)],text=True,capture_output=True)
  assert (p.returncode==0)==ok,(data,p.stdout,p.stderr)
  if ok:assert p.stdout.strip()=='32'
  checks+=1
print(f'PASS fdinfo builtin parser cases={checks}; synthetic metadata, not Android execution')
