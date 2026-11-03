from pathlib import Path
import tempfile,subprocess,hashlib,shutil,time
D=Path(__file__).resolve().parents[1]
source=(D/'live-targets.sh').read_text()
functions=source[source.index('digest_valid(){'):source.index('[ $# -gt 0 ]')]
# Synthetic public fixtures only; production runtime/ownership/namespace are mocked.
base=D/'tests/fixtures'
templates=[base/'generic.xml',base/'sku.xml',base/'sec.xml']
names=['generic','sku','sec']
checks=0
with tempfile.TemporaryDirectory(prefix='clarifae-routing-host-') as temp:
 R=Path(temp)
 for f in ('ram','state','base','live'): (R/f).mkdir()
 old=R/'ram/g-baseline';old.mkdir();(old/'targets').write_text('voice_call\n')
 rows=[]
 for i,(name,p) in enumerate(zip(names,templates)):
  b=p.read_bytes();h=hashlib.sha256(b).hexdigest();live=R/'live'/f'{name}.xml';live.write_bytes(b)
  (R/'base'/f'{name}.xml').write_bytes(b);(old/f'{name}.xml').write_bytes(b);(old/f'hash-{name}').write_text(h+'\n')
  (R/'base'/f'mount-{name}').write_text(str(100+i)+'\n');(R/'state'/f'mount-{name}').write_text(str(100+i)+'\n');(R/'base'/f'context-{name}').write_text('test-context\n');(R/'live'/f'{name}.xml.id').write_text(str(100+i)+'\n')
  rows.append(f'{name}|{live}|{h}')
 (R/'files').write_text('\n'.join(rows)+'\n');(R/'state/current').write_text(str(old)+'\n');(R/'state/managed').write_text('0\n');shutil.copyfile(D/'render-targets.awk',R/'render-targets.awk')
 (R/'counter').write_text('200\n')
 harness='set -eu\nset -f\nR='+repr(str(R))+'\nRAM=$R/ram\n'+functions+ r'''
file_sha(){ shasum -a256 "$1" | awk '{print $1}'; }
runtime(){ case "$(cat "$R/state/managed")" in 0|1)return 0;;*)return 1;;esac; }
runtime_environment(){ return 0; }
protected(){ return 0; } # Host metadata stub, never used in production.
services(){ return 0; }
config_text(){ cat "$R/config"; }
mid(){ cat "$1.id"; }
idle(){ [ ! -f "$R/busy" ]; }
chcon(){ return 0; }
timeout(){ shift;"$@"; } # Explicit host command stub.
toybox(){ [ ! -f "$R/fail-guard" ] || return 1;shift 5;exec bash "$R/harness.sh" "$@"; } # Real watcher, mocked launcher.
atomic_text(){
 if [ "$1" = "$R/state/managed" ] && [ -f "$R/corrupt-managed" ];then printf '' > "$1";rm "$R/corrupt-managed";return 1;fi
 printf '%s\n' "$2" > "$1.tmp.$$" && mv "$1.tmp.$$" "$1"
}
reload(){ echo reload >> "$R/reloads";if [ -f "$R/fail-reload" ];then rm "$R/fail-reload";return 1;fi;return 0; }
mount(){
 if [ "$1" != --bind ];then return 0;fi
 if [ -f "$R/fail-bind" ];then rm "$R/fail-bind";return 1;fi
 [ -f "$T/intent-bind-$(basename "$3" .xml)" ] || [ -f "$T/intent-restore-bind-$(basename "$3" .xml)" ] || return 1
 cp "$2" "$3";i=$(cat "$R/counter");i=$((i+1));echo "$i" > "$R/counter";echo "$i" > "$3.id"
 if [ -f "$R/fail-after-bind" ];then rm "$R/fail-after-bind";return 1;fi
 if [ -f "$R/race-guard" ];then
  rm "$R/race-guard";echo "$T" > "$R/race-transaction"
  bash "$R/harness.sh" guard "$T" > "$R/race-guard.log" 2>&1 &
  n=0;while [ ! -d "$T/decision" ];do sleep .02;n=$((n+1));[ "$n" -lt 100 ];done
 fi
}
umount(){ name=$(basename "$1" .xml);cp "$R/base/$name.xml" "$1";cp "$R/base/mount-$name" "$1.id"; }
case "$1" in guard)recover "$2" && release_lock "$2";;watch)watch "$2";;*)run "$1";;esac
'''
 (R/'harness.sh').write_text(harness)
 def config(value): (R/'config').write_text('enabled=1\nout_enabled=1\nout_targets='+value+'\n')
 def run(expect=0,mode='apply',blocked=False):
  global checks
  r=subprocess.run(['bash',str(R/'harness.sh'),mode],text=True,capture_output=True)
  assert (r.returncode==0)==(expect==0),(r.returncode,r.stdout,r.stderr)
  end=time.monotonic()+5
  if blocked:
   assert (R/'lock').exists(),r.stderr
   tx=Path((R/'lock/transaction').read_text().strip())
   while not (tx/'guard-terminal').exists() and time.monotonic()<end: time.sleep(.01)
   assert (tx/'guard-terminal').read_text().strip()=='blocked'
  else:
   while (R/'lock').exists() and time.monotonic()<end: time.sleep(.01)
   assert not (R/'lock').exists(),r.stderr
  checks+=1
  return r
 def targets():return (Path((R/'state/current').read_text().strip())/'targets').read_text().strip()
 config('music,voice_call');run();assert targets()=='music,voice_call';checks+=1
 for n in names:assert int((R/'live'/f'{n}.xml.id').read_text())>200
 count=(R/'counter').read_text();generations=sorted(p.name for p in (R/'ram').iterdir());run();assert (R/'counter').read_text()==count and sorted(p.name for p in (R/'ram').iterdir())==generations;checks+=1
 config('voice_call');run();assert targets()=='voice_call';checks+=1
 config('');run();assert targets()=='';checks+=1
 config('music');(R/'busy').touch();run(1);assert targets()=='';(R/'busy').unlink();checks+=1
 (R/'fail-bind').touch();run(1);assert targets()=='';checks+=1
 (R/'fail-reload').touch();run(1);assert targets()=='';checks+=1
 run();assert targets()=='music';checks+=1
 config('bad;command');run(1);assert targets()=='music';checks+=1
 config('music');run(mode='restart');assert targets()=='music';checks+=1
 config('voice_call');count=(R/'counter').read_text();(R/'fail-guard').touch();run(1);(R/'fail-guard').unlink();assert (R/'counter').read_text()==count and targets()=='music';checks+=1
 (R/'corrupt-managed').touch();run(1);assert targets()=='music' and (R/'state/managed').read_text().strip()=='1';checks+=1
 config('voice_call');(R/'race-guard').touch();run(1)
 tx=Path((R/'race-transaction').read_text().strip());assert (tx/'decision/result').read_text().strip()=='rollback' and targets()=='music';checks+=1
 (R/'live/generic.xml.id').write_text('99999\n');run(1);checks+=1
 (R/'live/generic.xml.id').write_text((R/'state/mount-generic').read_text())
 (R/'fail-after-bind').touch();run(1,blocked=True)
 tx=Path((R/'lock/transaction').read_text().strip());assert (tx/'intent-bind-generic').is_file() and (tx/'observed-failed-bind-generic').is_file() and (tx/'decision/result').read_text().strip()=='rollback-blocked';checks+=1
 print('PASS routing transactions checks='+str(checks)+' enable/disable/empty/idempotency/restart/busy/bind-failure/reload-failure/invalid/unowned/guard-ready/commit-write-failure/guard-race/ambiguous-bind-blocked; synthetic mocks + actual watcher state machine')
