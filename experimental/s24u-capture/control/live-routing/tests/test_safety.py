"""Host-only helper tests: mocked Android services, metadata and hashes; no device."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'live-targets.sh').read_text()
functions = source[source.index('digest_valid(){'):source.index('[ $# -gt 0 ]')]
checks = 0
with tempfile.TemporaryDirectory(prefix='clarifae-safety-') as temp:
    r = Path(temp)
    (r/'state').mkdir()
    (r/'ram').mkdir()
    boot = r/'current-boot'
    boot.write_text('synthetic-same-boot')
    (r/'boot').write_text('synthetic-same-boot')
    mountinfo = r/'mountinfo'
    mountinfo.write_text(f'1 0 0:1 / {r}/ram rw - tmpfs tmpfs rw' + chr(10))
    # Host test relocation only: production keeps literal Android proc paths.
    funcs = functions.replace('/proc/sys/kernel/random/boot_id', str(boot)).replace('/proc/1/mountinfo', str(mountinfo)).replace('/data/adb/clarifae/config.conf', str(r/'config'))
    def shell(code, overrides='', expected=0):
        global checks
        script = 'set -eu' + chr(10) + 'set -f' + chr(10) + f"R='{r}'" + chr(10) + 'RAM=$R/ram' + chr(10) + funcs + chr(10) + 'timeout(){ shift;"$@"; }' + chr(10) + overrides + chr(10) + code
        result = subprocess.run(['bash', '-c', script], text=True, capture_output=True, timeout=15)
        assert result.returncode == expected, (result.returncode, result.stdout, result.stderr, code)
        checks += 1
        return result.stdout

    runtime_mocks = '''
layout_valid(){ return 0; }
initialized(){ return 0; }
getenforce(){ echo "$SELINUX"; }
need(){ case "$FAIL:$1" in engine:*/soundfx/libclarifae.so|bridge:*/audio.primary.pineapple.so)return 1;;esac;return 0; }
'''
    for selinux, fail, outcome in [('Enforcing', '', 'good'), ('Permissive', '', 'blocked'),
                                    ('Enforcing', 'engine', 'blocked'), ('Enforcing', 'bridge', 'blocked')]:
        output = shell(f"SELINUX={selinux};FAIL='{fail}';if runtime;then echo good;else echo blocked;fi", runtime_mocks)
        assert output.strip() == outcome
    boot.write_text('different-synthetic-boot')
    assert shell("SELINUX=Enforcing;FAIL='';if runtime;then echo bad;else echo blocked;fi", runtime_mocks).strip() == 'blocked'
    boot.write_text('synthetic-same-boot')
    mountinfo.write_text('')
    assert shell("SELINUX=Enforcing;FAIL='';if runtime;then echo bad;else echo blocked;fi", runtime_mocks).strip() == 'blocked'
    assert shell('if initialized;then echo bad;else echo uninitialized;fi', 'protected(){ return 1; }').strip() == 'uninitialized'

    audio_ok = '- Actual mode = MODE_NORMAL' + chr(10) + '## RecordActivityMonitor' + chr(10) + '### Recording Activity' + chr(10)
    audio_cases = [
        (audio_ok, True),
        (audio_ok.replace('### Recording Activity', 'riid 1 active? false'+chr(10)+'AudioRecordingConfiguration session: 1'+chr(10)+'### Recording Activity'), True),
        (audio_ok.replace('MODE_NORMAL', 'MODE_IN_COMMUNICATION'), False),
        (audio_ok.replace('## RecordActivityMonitor', ''), False),
        (audio_ok.replace('### Recording Activity', ''), False),
        (audio_ok.replace('### Recording Activity', 'riid 1 active? true'+chr(10)+'### Recording Activity'), False),
        (audio_ok.replace('### Recording Activity', 'AudioRecordingConfiguration session: unknown'+chr(10)+'### Recording Activity'), False),
        (audio_ok.replace('### Recording Activity', 'active=true'+chr(10)+'### Recording Activity'), False),
        ('', False), (audio_ok + '- Actual mode = MODE_IN_CALL'+chr(10), False),
        (audio_ok.replace('### Recording Activity', 'riid 1'+chr(10)+'### Recording Activity'), False),
        (audio_ok.replace('### Recording Activity', 'active? true'+chr(10)+'### Recording Activity'), False),
        (audio_ok.replace('### Recording Activity', 'unrecognized record'+chr(10)+'### Recording Activity'), False),
        (audio_ok.replace('### Recording Activity', 'riid 1 active? false'+chr(10)+'### Recording Activity'), False),
        ('### Recording Activity'+chr(10)+audio_ok.replace('### Recording Activity',''), False),
    ]
    for text, ok in audio_cases:
        (r/'audio').write_text(text)
        output = shell('if idle;then echo idle;else echo blocked;fi', 'timeout(){ cat "$R/audio"; }')
        assert output.strip() == ('idle' if ok else 'blocked'), text
        assert not list((r/'state').glob('audio-gate-*')), 'raw gate dump retained'
    assert shell('if idle;then echo bad;else echo blocked;fi', 'timeout(){ return 1; }').strip() == 'blocked'

    rows = [('generic','/vendor/etc/audio_effects.xml'), ('sku','/vendor/etc/audio/sku_pineapple/audio_effects.xml'), ('sec','/vendor/etc/audio_effects_sec.xml')]
    manifest = ''.join(n+'|'+p+'|'+'a'*64+chr(10) for n,p in rows)
    for text, ok in [(manifest,True), (manifest+manifest.splitlines()[0]+chr(10),False),
                     (manifest.replace('a'*64,'bad'),False), (manifest.replace('generic|','other|'),False),
                     (manifest.replace('/vendor/etc/audio_effects.xml','/unexpected/target'),False)]:
        (r/'files').write_text(text)
        output = shell('if manifest_valid;then echo valid;else echo blocked;fi')
        assert output.strip() == ('valid' if ok else 'blocked')

    regular = r/'regular'
    regular.write_text('synthetic')
    (r/'link').symlink_to(regular)
    stat_mock = 'stat(){ if [ "$2" = %u ];then echo "$OWNER";else echo "$MODE";fi; }'
    for name, owner, mode, ok in [('regular',0,'600',True), ('regular',1000,'600',False),
                                  ('regular',0,'666',False), ('link',0,'600',False), ('missing',0,'600',False)]:
        output = shell(f'OWNER={owner};MODE={mode};if protected "$R/{name}";then echo valid;else echo blocked;fi', stat_mock)
        assert output.strip() == ('valid' if ok else 'blocked')

    shell('MUTATOR_PID=;mutate true;[ -z "$MUTATOR_PID" ]')
    shell('MUTATOR_PID=;if mutate false;then exit 1;fi;[ -z "$MUTATOR_PID" ]')
    shell('MUTATOR_PID=;MUTATOR_UNCERTAIN=0;if mutate true;then exit 1;fi;[ "$MUTATOR_UNCERTAIN" = 1 ]', 'timeout(){ return 124; }')
    shell('MUTATOR_PID=;if mutate slow;then exit 1;fi;[ -n "$MUTATOR_PID" ];kill -0 "$MUTATOR_PID";builtin wait "$MUTATOR_PID"',
          'wait(){ return 143; }'+chr(10)+'slow(){ sleep 1; }')

    selections = [('1','1','media,calls,notif','music,voice_call,notification'),
                  ('1','1','',''), ('0','1','music',''), ('1','0','music',''),
                  ('1','1','music,music','music'), ('1','1','music,',None),
                  ('1','1','bad;command',None), ('2','1','music',None)]
    for master, side, targets, expected in selections:
        (r/'config').write_text('enabled='+master+chr(10)+'out_enabled='+side+chr(10)+'out_targets='+targets+chr(10))
        result=shell('if chosen=$(selected);then printf "ok:%s" "$chosen";else printf blocked;fi')
        assert result == ('blocked' if expected is None else 'ok:'+expected)
    (r/'config').write_text('enabled=1'+chr(10)+'out_enabled=1'+chr(10))
    assert shell('if selected;then echo bad;else echo blocked;fi').strip()=='blocked'

    # The real recovery mutex must wait for prior forward mutation, not race it.
    shell('''
T=$R/state/tx-race;mkdir "$T";mutation_lock "$T"
recover "$T" & guard=$!
n=0;while [ ! -d "$T/decision" ];do sleep .02;n=$((n+1));[ "$n" -lt 100 ];done
[ ! -e "$R/events" ];echo forward-done > "$R/events"
mutation_unlock "$T";wait "$guard"
[ "$(cat "$T/decision/result")" = rollback ]
[ "$(cat "$R/events" | tr '\n' ',')" = forward-done,restore, ]
''', 'protected(){ return 0; }'+chr(10)+'restore(){ echo restore >> "$R/events"; }')
    shell('''
T=$R/state/tx-stuck;mkdir "$T";mutation_lock "$T"
mkdir "$R/lock";echo "$T" > "$R/lock/transaction"
if recover "$T";then exit 1;fi
[ "$(cat "$T/decision/result")" = rollback-blocked ]
[ -d "$R/lock" ];[ -d "$T/mutation" ];[ ! -e "$R/unsafe-restore" ]
mutation_unlock "$T"
''', 'protected(){ return 0; }'+chr(10)+'sleep(){ return 0; }'+chr(10)+'restore(){ touch "$R/unsafe-restore"; }')
    shell('if need missing "";then exit 1;fi', 'sha256sum(){ return 1; }')
    shell('if file_sha missing;then exit 1;fi', 'sha256sum(){ return 1; }')
    shell('if need missing '+('a'*64)+';then exit 1;fi', 'sha256sum(){ echo bad; }')
    shell('need synthetic '+('a'*64), 'sha256sum(){ echo '+('a'*64)+'  synthetic; }')
    shell('''
T=$R/state/tx-env;mkdir "$T";echo "$RAM/g-old" > "$T/old";echo 0 > "$T/old-managed"
if restore "$T";then exit 1;fi;[ ! -e "$R/forbidden-mutation" ]
''', 'snapshot_valid(){ return 0; }'+chr(10)+'runtime_environment(){ return 1; }'+chr(10)+'mutate(){ touch "$R/forbidden-mutation"; }')
    shell('''
T=$R/state/tx-incomplete;mkdir "$T" "$T/decision"
if watch "$T";then exit 1;fi
[ "$(cat "$T/guard-terminal")" = blocked ]
''', 'protected(){ return 0; }'+chr(10)+'sleep(){ return 0; }')
    for label, valid in [('good',True), ('invalid',False)]:
        shell(f'''
T=$R/state/tx-terminal-{label};mkdir "$T" "$T/decision"
echo commit > "$T/decision/result"
if watch "$T";then result=commit;else result=blocked;fi
[ "$result" = {('commit' if valid else 'blocked')} ]
[ "$(cat "$T/guard-terminal")" = "$result" ]
''', 'protected(){ return 0; }'+chr(10)+f'commit_verified(){{ return {0 if valid else 1}; }}')
    shell('''
T=$R/state/tx-launchfail;mkdir "$T"
if start_guard;then exit 1;fi;[ ! -f "$T/guard-ready" ]
''', 'toybox(){ return 1; }')
    shell('''
T=$R/state/tx-noack;mkdir "$T"
if start_guard;then exit 1;fi;builtin wait "$watch_pid"
if await_guard_commit;then exit 1;fi
''', 'toybox(){ command sleep 1; }'+chr(10)+'sleep(){ return 0; }')
    # Two releasers race while the winner pauses after ownership checks. A
    # successor is installed immediately after rmdir; neither loser may delete it.
    shell('''
command rm "$R/lock/transaction";command rmdir "$R/lock"
T=$R/state/tx-release;mkdir "$T" "$T/decision" "$R/lock";echo "$T" > "$R/lock/transaction"
echo rollback > "$T/decision/result";echo restored > "$T/result"
rm(){
 echo entered >> "$R/release-count";touch "$R/release-entered"
 while [ ! -e "$R/release-continue" ];do sleep .02;done
 command rm "$@"
}
rmdir(){
 command rmdir "$@"
 if [ "$1" = "$R/lock" ];then mkdir "$R/lock";echo successor > "$R/lock/transaction";fi
}
release_lock "$T" & first=$!
n=0;while [ ! -e "$R/release-entered" ];do sleep .02;n=$((n+1));[ "$n" -lt 100 ];done
release_lock "$T" & second=$!;wait "$second"
[ "$(wc -l < "$R/release-count" | tr -d ' ')" = 1 ]
touch "$R/release-continue";wait "$first"
release_lock "$T";[ "$(cat "$R/lock/transaction")" = successor ]
''', 'protected(){ return 0; }')
    shell('''
T=$R/state/tx-claim-race;mkdir "$T";touch "$T/abort"
mkdir(){
 if [ "$1" = "$T/decision" ];then command mkdir "$T/decision";return 1;fi
 command mkdir "$@"
}
if watch "$T";then exit 1;fi
[ "$(cat "$T/guard-terminal")" = blocked ]
[ ! -e "$R/unsafe-release" ];[ ! -e "$R/unsafe-recovery" ];[ -d "$R/lock" ]
''', 'protected(){ return 0; }'+chr(10)+'restore(){ touch "$R/unsafe-recovery"; }'+chr(10)+'release_lock(){ touch "$R/unsafe-release"; }')
    shell('''
T=$R/state/tx-pending-release;mkdir "$T" "$T/decision"
if release_lock "$T";then exit 1;fi
[ "$(cat "$R/lock/transaction")" = successor ];[ ! -d "$T/release-owner" ]
''', 'protected(){ return 0; }')
print(f'PASS manager safety checks={checks}; runtime/strict-idle/hash/metadata/mutator/guard-terminal/successor-lock; host mocks only')
