#!/bin/sh
# Host-only fixture: no real Android property, service or XML mutations.
# Run with sh, or TEST_SH=/path/to/another/posix-shell sh tests/test_control.sh.
set -u
HERE=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd) || exit 1
TEST_SH=${TEST_SH:-/bin/sh}
if [ -e /system/bin/setprop ] || [ -e /data/adb/clarifae/functions.sh ]; then
  echo 'Refusing host fixture on Android or with an installed Clarifae helper' >&2
  exit 1
fi
mkdir -p "$HERE/build" || exit 1
FIX=$(mktemp -d "$HERE/build/control-fixture.XXXXXX") || exit 1
trap 'rm -rf "$FIX"' 0
trap 'exit 1' 1 2 3 15
mkdir -p "$FIX/common" "$FIX/bin" "$FIX/state" || exit 1
TEST_HELPERS="$HERE/common/functions.sh"
TEST_WRAPPER="$FIX/common/functions.sh"
TEST_STATE="$FIX/state"
TEST_BIN="$FIX/bin"
TEST_PROP_LOG="$FIX/properties.log"
TEST_ANDROID_LOG="$FIX/android.log"
TEST_REAL_MV=$(command -v mv)
TEST_REAL_AWK=$(command -v awk)
TEST_REAL_GREP=$(command -v grep)
export TEST_HELPERS TEST_WRAPPER TEST_STATE TEST_BIN TEST_PROP_LOG TEST_ANDROID_LOG
export TEST_REAL_MV TEST_REAL_AWK TEST_REAL_GREP
cp "$HERE/common/clarifae-ctl" "$FIX/common/clarifae-ctl" || exit 1
cat > "$TEST_WRAPPER" <<'EOF'
. "$TEST_HELPERS"
CLARIFAE_DIR="$TEST_STATE"
CONFIG="$TEST_STATE/config.conf"
LOGFILE="$TEST_STATE/clarifae.log"
PATCH_MARK="$TEST_STATE/patched"
BACKUP="$TEST_STATE/backup"
MODULE_DIR="$TEST_STATE/module"
find_audio_effects_file() { return 1; }
[ "${TEST_FAIL_INIT:-}" != 1 ] || conf_init() { return 1; }
[ "${TEST_FAIL_DEFAULTS:-}" != 1 ] || clarifae_defaults() { return 1; }
EOF
cat > "$TEST_BIN/setprop" <<'EOF'
#!/bin/sh
printf '%s|%s|<%s>\n' "${TEST_PROP_TOOL:-setprop}" "$1" "$2" >> "$TEST_PROP_LOG"
if [ "${TEST_FAIL_PROP:-}" = "$1" ] || [ "${TEST_FAIL_PROP:-}" = all ]; then
  echo "fixture property failure: $1" >&2
  exit 42
fi
EOF
cat > "$TEST_BIN/resetprop" <<'EOF'
#!/bin/sh
[ "$#" -eq 4 ] && [ "$1" = -n ] && [ "$2" = -p ] || exit 43
TEST_PROP_TOOL=resetprop exec "$TEST_BIN/setprop" "$3" "$4"
EOF
cat > "$TEST_BIN/mv" <<'EOF'
#!/bin/sh
[ "${TEST_FAIL_MV:-}" != 1 ] || exit 44
exec "$TEST_REAL_MV" "$@"
EOF
cat > "$TEST_BIN/awk" <<'EOF'
#!/bin/sh
[ "${TEST_FAIL_AWK:-}" != 1 ] || exit 45
exec "$TEST_REAL_AWK" "$@"
EOF
cat > "$TEST_BIN/grep" <<'EOF'
#!/bin/sh
[ "${TEST_FAIL_GREP:-}" != 1 ] || exit 2
exec "$TEST_REAL_GREP" "$@"
EOF
for tool in pidof dumpsys logcat killall stop start; do
  cat > "$TEST_BIN/$tool" <<'EOF'
#!/bin/sh
printf '%s:%s\n' "${0##*/}" "$*" >> "$TEST_ANDROID_LOG"
exit 1
EOF
done
chmod +x "$TEST_BIN/"* || exit 1
PATH="$TEST_BIN:$PATH"; export PATH
CONFIG="$TEST_STATE/config.conf"
OUT="$FIX/stdout"; ERR="$FIX/stderr"
checks=0; failures=0; RC=0
TEST_FAIL_INIT= TEST_FAIL_DEFAULTS= TEST_FAIL_PROP= TEST_FAIL_MV= TEST_FAIL_AWK= TEST_FAIL_GREP=
export TEST_FAIL_INIT TEST_FAIL_DEFAULTS TEST_FAIL_PROP TEST_FAIL_MV TEST_FAIL_AWK TEST_FAIL_GREP
check() {
  label=$1; shift; checks=$((checks + 1))
  if "$@"; then :; else echo "FAIL $checks: $label" >&2; failures=$((failures + 1)); fi
}
run_ctl() { "$TEST_SH" "$FIX/common/clarifae-ctl" "$@" > "$OUT" 2> "$ERR"; RC=$?; }
run_helper() { "$TEST_SH" -c '. "$TEST_WRAPPER"; "$@"' fixture-helper "$@" > "$OUT" 2> "$ERR"; RC=$?; }
expect_rc() { check "$1" test "$RC" -eq "$2"; }
expect_out() { check "$1" test "$(cat "$OUT")" = "$2"; }
contains() { check "$1" "$TEST_REAL_GREP" -Fq -- "$2" "$3"; }
not_contains() { checks=$((checks + 1)); if "$TEST_REAL_GREP" -Fq -- "$2" "$3"; then echo "FAIL $checks: $1" >&2; failures=$((failures + 1)); fi; }
fresh() {
  TEST_FAIL_INIT= TEST_FAIL_DEFAULTS= TEST_FAIL_PROP= TEST_FAIL_MV= TEST_FAIL_AWK= TEST_FAIL_GREP=
  "$TEST_SH" -c '. "$TEST_WRAPPER"; clarifae_defaults > "$CONFIG"' || exit 1
  : > "$TEST_PROP_LOG"; : > "$TEST_ANDROID_LOG"
}

run_helper conf_get; expect_rc 'helper get arity required' 1
run_helper conf_set out_targets; expect_rc 'helper set requires explicit value' 1
run_helper conf_get out_targets
expect_rc 'missing file read defaults' 0
expect_out 'missing file output default unchanged' 'music,voice_call'
printf 'enabled=1\n' > "$CONFIG"
run_helper conf_get out_targets
expect_out 'missing key falls back' 'music,voice_call'
printf 'out_targets=\n' >> "$CONFIG"
run_helper conf_get out_targets
expect_rc 'explicit empty readable' 0
expect_out 'explicit empty does not fall back' ''
printf 'out_targets=music\nout_targets=\n' > "$CONFIG"
run_helper conf_get out_targets
expect_out 'last duplicate can be explicitly empty' ''
fresh
for key in 'unknown' 'enabled.*' 'enabled in_enabled' ''; do
  cp "$CONFIG" "$FIX/before"
  run_helper conf_get "$key"; expect_rc "unknown get key rejected: $key" 1
  run_helper conf_set "$key" 1; expect_rc "unknown set key rejected: $key" 1
  check 'invalid key leaves config intact' cmp -s "$CONFIG" "$FIX/before"
done
value="a'b&|/\\=\$(printf injected) *?[] -n"
run_helper conf_set out_targets "$value"
expect_rc 'literal helper value saves' 0
run_helper conf_get out_targets
expect_out 'literal metacharacters preserved without sed/awk interpretation' "$value"
cp "$CONFIG" "$FIX/before"
newline='music
voice_call'
run_helper conf_set out_targets "$newline"; expect_rc 'helper rejects newline injection' 1
cr=$(printf '\r')
run_helper conf_set out_targets "music${cr}voice_call"; expect_rc 'helper rejects CR injection' 1
check 'line injection leaves config intact' cmp -s "$CONFIG" "$FIX/before"
printf 'out_targets=music\nother=preserved\nout_targets=voice_call\n' > "$CONFIG"
run_helper conf_set out_targets ''
expect_rc 'replace duplicate keys with empty' 0
check 'replacement leaves a single key' test "$("$TEST_REAL_GREP" -c '^out_targets=' "$CONFIG")" -eq 1
contains 'unrelated lines preserved' 'other=preserved' "$CONFIG"
run_helper conf_get out_targets; expect_out 'empty replacement reads back empty' ''

fresh
for csv in mic voice_comm camcorder voice_rec 'mic,voice_comm,camcorder,voice_rec' 'mic,mic'; do
  run_ctl set targets "$csv"; expect_rc "valid capture CSV $csv" 0
done
for csv in '' music voice_call ring alarm notification system media calls notif 'music,voice_call,ring,alarm,notification,system'; do
  run_ctl set out_targets "$csv"; expect_rc "valid playback CSV $csv" 0
done
cp "$CONFIG" "$FIX/before"; : > "$TEST_PROP_LOG"
for key in targets out_targets; do
  for csv in ',music' 'music,' 'mic,,voice_comm' 'not_a_route' 'music mic' 'mic;echo bad' 'mic*' 'mic=1' "$newline" "music${cr}"; do
    run_ctl set "$key" "$csv"; expect_rc "invalid $key CSV rejected" 1
  done
done
for csv in ',mic' 'mic,' 'mic,,voice_comm'; do
  run_ctl set targets "$csv"; expect_rc 'capture comma structure rejected' 1
done
for csv in ',music' 'music,' 'music,,voice_call'; do
  run_ctl set out_targets "$csv"; expect_rc 'playback comma structure rejected' 1
done
run_ctl set targets ''; expect_rc 'empty capture rejected' 1
run_ctl set targets music; expect_rc 'playback token rejected on capture' 1
run_ctl set out_targets mic; expect_rc 'capture token rejected on playback' 1
check 'all invalid CSV writes leave config unchanged' cmp -s "$CONFIG" "$FIX/before"
check 'invalid CSV sends no properties' test ! -s "$TEST_PROP_LOG"
for value in '' -1 +1 101 01 00 '1 ' ' 1' 1.5 9999999999999999999999; do
  run_ctl set strength "$value"; expect_rc 'invalid JSON/range number rejected' 1
done
run_ctl set strength 0; expect_rc 'numeric lower bound' 0
run_ctl set strength 100; expect_rc 'numeric upper bound' 0
run_ctl set atten_db 60; expect_rc 'attenuation upper bound' 0
run_ctl set atten_db 61; expect_rc 'attenuation above bound rejected' 1
run_ctl set enabled 2; expect_rc 'boolean domain enforced' 1
run_ctl set mode 4; expect_rc 'mode domain enforced' 1
run_ctl set loglevel 4; expect_rc 'loglevel domain enforced' 1
run_ctl set out_targets; expect_rc 'omitted value not confused with explicit empty' 2
run_ctl set out_targets '' extra; expect_rc 'extra argument rejected' 2
run_ctl set-many; expect_rc 'empty batch rejected' 2
run_ctl set-many enabled; expect_rc 'assignment without equals rejected' 1
run_ctl set-many 'out_targets=' 'strength=75'; expect_rc 'valid batch retains empty' 0
run_ctl set-many 'mode=1' 'unknown=1'; expect_rc 'partial batch error propagates' 1
run_ctl get-json; expect_rc 'settings JSON succeeds' 0
contains 'get-json preserves explicit empty' '"out_targets":""' "$OUT"
run_ctl status-json; expect_rc 'status JSON succeeds' 0
contains 'status-json preserves explicit empty' '"out_targets":""' "$OUT"
: > "$TEST_PROP_LOG"
run_ctl apply; expect_rc 'Apply sync succeeds' 0
contains 'Apply makes only settings-sync claim' 'settings synced; installed target routes unchanged' "$OUT"
contains 'property sync retains empty as an argument' 'persist.clarifae.out_targets|<>' "$TEST_PROP_LOG"
check 'Apply attempts all thirteen keys' test "$(wc -l < "$TEST_PROP_LOG" | tr -d ' ')" -eq 13
not_contains 'Apply does not request restart' '|ctl.restart|' "$TEST_PROP_LOG"

fresh; cp "$CONFIG" "$FIX/before"
TEST_FAIL_MV=1
run_ctl set strength 81; expect_rc 'conf_set rename failure propagates' 1
check 'failed config rename preserves original' cmp -s "$CONFIG" "$FIX/before"
check 'failed config sends no property' test ! -s "$TEST_PROP_LOG"
expect_out 'failed save has no successful assignment output' ''
contains 'config failure explained' 'could not save strength' "$ERR"
check 'failed rename cleans temporary file' test "$(find "$TEST_STATE" -name 'config.conf.tmp.*' | wc -l | tr -d ' ')" -eq 0
TEST_FAIL_MV=; TEST_FAIL_AWK=1
run_ctl set strength 82; expect_rc 'conf_set writer failure propagates' 1
check 'failed writer preserves original' cmp -s "$CONFIG" "$FIX/before"
TEST_FAIL_AWK=; TEST_FAIL_PROP=persist.clarifae.strength
run_ctl set strength 83; expect_rc 'property failure propagates after saving' 1
contains 'partial save remains visible on disk' 'strength=83' "$CONFIG"
contains 'partial save is explained' 'saved strength, but property sync failed' "$ERR"
expect_out 'property failure has no success assignment' ''
TEST_FAIL_PROP=persist.clarifae.enabled; : > "$TEST_PROP_LOG"
run_ctl apply; expect_rc 'early prop_sync failure not hidden by later successes' 1
check 'sync still tries all keys' test "$(wc -l < "$TEST_PROP_LOG" | tr -d ' ')" -eq 13
expect_out 'failed Apply never prints sync success' ''
contains 'Apply partial failure explained' 'settings sync failed (possibly partial)' "$ERR"
TEST_FAIL_PROP=; TEST_FAIL_GREP=1; : > "$TEST_PROP_LOG"
run_helper conf_get out_targets; expect_rc 'config read error not mistaken for absent key' 1
run_helper prop_sync; expect_rc 'config read error propagates from sync' 1
check 'unreadable config does not sync fallback properties' test ! -s "$TEST_PROP_LOG"
TEST_FAIL_GREP=; TEST_FAIL_INIT=1
for command in get get-json status-json status apply reset restart-audio; do
  run_ctl "$command"; expect_rc "initialization failure propagates for $command" 1
done
TEST_FAIL_INIT=

fresh
TEST_FAIL_DEFAULTS=1; cp "$CONFIG" "$FIX/before"
run_ctl reset; expect_rc 'failed defaults generation propagates' 1
check 'failed defaults generation preserves original' cmp -s "$CONFIG" "$FIX/before"
TEST_FAIL_DEFAULTS=; TEST_FAIL_MV=1
run_ctl reset; expect_rc 'failed reset rename propagates' 1
check 'failed reset rename preserves original' cmp -s "$CONFIG" "$FIX/before"
TEST_FAIL_MV=; TEST_FAIL_PROP=persist.clarifae.enabled
run_ctl reset; expect_rc 'reset property failure propagates' 1
expect_out 'failed reset never prints success' ''
TEST_FAIL_PROP=
run_ctl reset; expect_rc 'reset succeeds' 0
contains 'reset routes limitation explicit' 'installed target routes unchanged' "$OUT"
run_helper conf_get out_targets; expect_out 'default playback policy unchanged' 'music,voice_call'

fresh
run_ctl restart-audio; expect_rc 'restart request accepted' 0
contains 'restart output request-only and unverified' 'restart requested (completion not verified)' "$OUT"
contains 'restart does not claim regenerated routes' 'installed target routes unchanged' "$OUT"
check 'restart request exactly once' test "$("$TEST_REAL_GREP" -c '|ctl.restart|<audioserver>' "$TEST_PROP_LOG")" -eq 1
check 'no unconditional kill/stop/start fallback' test ! -s "$TEST_ANDROID_LOG"
TEST_FAIL_PROP=ctl.restart; : > "$TEST_PROP_LOG"
run_ctl restart-audio; expect_rc 'restart request failure propagates' 1
expect_out 'restart request failure never prints success' ''
contains 'restart error explained' 'audioserver restart request failed' "$ERR"
check 'restart failure attempts no fallbacks' test ! -s "$TEST_ANDROID_LOG"
TEST_FAIL_PROP=persist.clarifae.enabled; : > "$TEST_PROP_LOG"
run_ctl restart-audio; expect_rc 'failed sync prevents restart request' 1
not_contains 'no restart after failed sync' '|ctl.restart|' "$TEST_PROP_LOG"
contains 'failed sync restart refusal explained' 'audio restart not requested' "$ERR"
TEST_FAIL_PROP=; "$TEST_REAL_MV" "$TEST_BIN/resetprop" "$TEST_BIN/resetprop.off"
run_ctl set out_targets ''; expect_rc 'setprop fallback works without resetprop' 0
contains 'fallback preserves empty value' 'setprop|persist.clarifae.out_targets|<>' "$TEST_PROP_LOG"
TEST_FAIL_PROP=persist.clarifae.out_targets
run_ctl set out_targets ''; expect_rc 'setprop fallback failure propagates' 1

if [ "$failures" -ne 0 ]; then
  echo "FAIL POSIX control fixture checks=$checks failures=$failures shell=$TEST_SH" >&2
  exit 1
fi
printf 'PASS POSIX control fixture checks=%s shell=%s no_hardware=1\n' "$checks" "$TEST_SH"
