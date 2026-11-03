"""Host-only optional control mirrors/manager hooks; never executes Android commands."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
if Path('/system/bin/setprop').exists() or Path('/data/adb/clarifae/functions.sh').exists():
    raise SystemExit('Host fixture refuses an Android/installed-helper environment')
helpers=ROOT/'integration/functions.sh'
cli=(ROOT/'integration/clarifae-ctl').read_text()
functions=cli[cli.index('jbool(){'):] if 'jbool(){' in cli else cli[cli.index('jbool() {'):]
functions=functions[:functions.index('case "$1" in'+chr(10)+'  get)')]
checks=0
with tempfile.TemporaryDirectory(prefix='clarifae-optin-') as temp:
    r=Path(temp)
    fake=r/'manager.sh'
    fake.write_text('echo "$1" >> "$TEST_ROOT/manager-calls"'+chr(10)+'exit "$MANAGER_RC"'+chr(10))
    fake.chmod(0o600)
    (r/'link').symlink_to(fake)
    setup=f'. "{helpers}"'+chr(10)+functions+chr(10)+f"TEST_ROOT='{r}'"+chr(10)+'''
export TEST_ROOT
CLARIFAE_DIR=$TEST_ROOT;CONFIG=$TEST_ROOT/config;LOGFILE=$TEST_ROOT/log
LIVE_ROUTES=$TEST_ROOT/manager.sh
MANAGER_RC=0;export MANAGER_RC
OWNER=0;MODE=600;FAIL_VENDOR=;FAIL_LEGACY=
stat(){ if [ "$2" = %u ];then echo "$OWNER";else echo "$MODE";fi; }
_setp_vendor(){ echo "vendor|$1|<$2>" >> "$TEST_ROOT/properties";[ "$FAIL_VENDOR" != "$1" ]; }
_setp_legacy(){ echo "legacy|$1|<$2>" >> "$TEST_ROOT/properties";[ "$FAIL_LEGACY" != "$1" ]; }
clarifae_defaults > "$CONFIG"
: > "$TEST_ROOT/properties";rm -f "$TEST_ROOT/manager-calls"
'''
    def shell(code):
        global checks
        result=subprocess.run(['sh','-c',setup+chr(10)+code],text=True,capture_output=True,timeout=10)
        assert result.returncode==0,(code,result.returncode,result.stdout,result.stderr)
        checks+=1
        return result.stdout
    shell('conf_set out_targets "";[ "$(conf_get out_targets)" = "" ]')
    shell('cmd_set out_targets "";grep -F "vendor|persist.vendor.audio.clarifae.out_targets|<>" "$TEST_ROOT/properties";grep -F "legacy|persist.clarifae.out_targets|<>" "$TEST_ROOT/properties"')
    shell('FAIL_VENDOR=persist.vendor.audio.clarifae.out_targets;if cmd_set out_targets music;then exit 1;fi;grep -F "vendor|persist.vendor.audio.clarifae.adapter|<0>" "$TEST_ROOT/properties";! grep -q "legacy|persist.clarifae.out_targets" "$TEST_ROOT/properties"')
    shell('FAIL_LEGACY=persist.clarifae.out_targets;if cmd_set out_targets music;then exit 1;fi;grep -F "vendor|persist.vendor.audio.clarifae.adapter|<0>" "$TEST_ROOT/properties"')
    shell('FAIL_VENDOR=persist.vendor.audio.clarifae.enabled;if cmd_apply;then exit 1;fi;[ ! -f "$TEST_ROOT/manager-calls" ]')
    shell('cmd_apply;[ "$(cat "$TEST_ROOT/manager-calls")" = apply ]')
    shell('MANAGER_RC=7;export MANAGER_RC;cmd_apply;[ "$?" = 7 ]')
    shell('cmd_restart_audio;[ "$(cat "$TEST_ROOT/manager-calls")" = restart ]')
    shell('LIVE_ROUTES=$TEST_ROOT/absent;if cmd_apply;then exit 1;fi;[ ! -f "$TEST_ROOT/manager-calls" ]')
    shell('LIVE_ROUTES=$TEST_ROOT/link;if cmd_apply;then exit 1;fi;[ ! -f "$TEST_ROOT/manager-calls" ]')
    shell('MODE=666;if cmd_apply;then exit 1;fi;[ ! -f "$TEST_ROOT/manager-calls" ]')
    shell('OWNER=1000;if cmd_apply;then exit 1;fi;[ ! -f "$TEST_ROOT/manager-calls" ]')
    shell('conf_init(){ return 1; };if cmd_apply;then exit 1;fi;[ ! -f "$TEST_ROOT/manager-calls" ]')
    shell('cmd_reset;[ ! -f "$TEST_ROOT/manager-calls" ]')
print(f'PASS optional control integration cases={checks}; mocked properties/ownership/manager only')
