#!/system/bin/sh
# Device-specific same-boot XML routing, not a cold-boot installer.
set -eu
set -f
umask 077
R=/data/adb/clarifae/live-targets-20261103-v1
RAM=$R/ram
[ "$(id -u)" = 0 ] || { echo 'Root required' >&2;exit 2; }
SELF_NS=$(readlink /proc/self/ns/mnt);INIT_NS=$(readlink /proc/1/ns/mnt)
[ -n "$SELF_NS" ] && [ -n "$INIT_NS" ] || { echo 'Mount namespace unavailable' >&2;exit 2; }
if [ "$SELF_NS" != "$INIT_NS" ];then exec toybox nsenter -t 1 -m -- sh "$0" "$@";fi
digest_valid(){ [ "${#1}" = 64 ] || return 1;case "$1" in *[!0-9a-f]*)return 1;;esac; }
file_sha(){
 _sha_line=$(sha256sum "$1") || return 1
 _sha_value=${_sha_line%% *};digest_valid "$_sha_value" || return 1
 printf '%s' "$_sha_value"
}
need(){
 digest_valid "$2" || return 1
 _actual_sha=$(file_sha "$1") || return 1
 [ "$_actual_sha" = "$2" ] || { echo "Hash refused: $1" >&2;return 1; }
}
mid(){
 # fdinfo identifies the VISIBLE mount, not the last textual mountinfo row.
 exec 9< "$1" || return 1
 _mid_id=$(awk '$1=="mnt_id:"{id=$2;n++}END{if(n!=1)exit 1;print id}' /proc/self/fdinfo/9) || { exec 9<&-;return 1; }
 exec 9<&-
 positive_id "$_mid_id" || return 1
 awk -v id="$_mid_id" -v p="$1" '$1==id&&$5==p{n++}END{exit n!=1}' /proc/1/mountinfo || return 1
 printf '%s' "$_mid_id"
}
atomic_text(){ printf '%s\n' "$2" > "$1.tmp.$$" && mv "$1.tmp.$$" "$1"; }
positive_id(){ case "$1" in ''|0|*[!0-9]*)return 1;;esac; }
protected(){
 [ -e "$1" ] && [ ! -L "$1" ] || return 1
 [ "$(stat -c %u "$1")" = 0 ] || return 1
 stat -c %a "$1" | grep -Eq '^[0-7]*[0145][0145]$'
}
manifest_valid(){
 awk -F '|' '
 NF!=3 || length($3)!=64 || $3~/[^0-9a-f]/ {bad=1}
 $1=="generic" && $2=="/vendor/etc/audio_effects.xml" {g++;next}
 $1=="sku" && $2=="/vendor/etc/audio/sku_pineapple/audio_effects.xml" {s++;next}
 $1=="sec" && $2=="/vendor/etc/audio_effects_sec.xml" {c++;next}
 {bad=1}
 END {exit bad || NR!=3 || g!=1 || s!=1 || c!=1}' "$R/files"
}
snapshot_path_valid(){
 case "$1" in "$RAM"/g-*) ;;*)return 1;;esac
 case "${1#"$RAM"/g-}" in ''|*[!A-Za-z0-9-]*)return 1;;esac
 [ "$(readlink -f "$1")" = "$1" ] && [ -d "$1" ] && protected "$1"
}
snapshot_valid(){
 snapshot_path_valid "$1" && protected "$1/targets" || return 1
 snapshot=$1
 while IFS='|' read -r name path base;do
  protected "$snapshot/$name.xml" && protected "$snapshot/hash-$name" && need "$snapshot/$name.xml" "$(cat "$snapshot/hash-$name")" || return 1
 done < "$R/files"
}
layout_valid(){
 # No bootstrap: an audited deployer must supply root-owned, non-writable
 # baseline/current metadata in this exact same-boot RAM snapshot.
 for item in "$R" "$RAM" "$R/base" "$R/state" "$R/boot" "$R/files" "$R/render-targets.awk";do protected "$item" || return 1;done
 manifest_valid || return 1
 while IFS='|' read -r name path base;do
  for item in "$R/base/$name.xml" "$R/base/mount-$name" "$R/base/context-$name";do protected "$item" || return 1;done
  positive_id "$(cat "$R/base/mount-$name")" && need "$R/base/$name.xml" "$base" || return 1
 done < "$R/files"
}
initialized(){
 protected "$R/state/current" && protected "$R/state/managed" || return 1
 case "$(cat "$R/state/managed")" in 0|1);;*)return 1;;esac
 snapshot_valid "$(current)" || return 1
 while IFS='|' read -r name path base;do protected "$R/state/mount-$name" && positive_id "$(cat "$R/state/mount-$name")" || return 1;done < "$R/files"
}
runtime_environment(){
 layout_valid || { echo 'Uninitialized or untrusted routing state; audited deployment required' >&2;return 1; }
 [ "$(cat /proc/sys/kernel/random/boot_id)" = "$(cat "$R/boot")" ] || { echo 'New boot; reactivation needs inspection' >&2;return 1; }
 [ "$(timeout 2 getenforce)" = Enforcing ] || return 1
 grep -q " $RAM .* - tmpfs " /proc/1/mountinfo || return 1
 need /vendor/lib64/soundfx/libclarifae.so 64ebebbfe8f62ed7a6c7f43cc558e710e916509dad9148191f9eec4f315811e9 || return 1
 need /vendor/lib64/hw/audio.primary.pineapple.so 5b889b6abed6b4fa73c28b1bf2c78983d69fc9f3f245e279b0badfbc94c671b6 || return 1
}
runtime(){ runtime_environment && initialized; }
current(){ cat "$R/state/current"; }
verify_xml(){
 V=$(current)
 while IFS='|' read -r name path base;do
  need "$path" "$(cat "$V/hash-$name")" || return 1
  [ "$(mid "$path")" = "$(cat "$R/state/mount-$name")" ] || { echo "Unowned mount: $path" >&2;return 1; }
 done < "$R/files"
}
services(){ [ "$(timeout 2 getprop init.svc.audioserver)" = running ] && [ "$(timeout 2 getprop init.svc.vendor.audio-hal)" = running ]; }
idle(){
 audio_gate="$R/state/audio-gate-$$.txt"
 timeout 8 dumpsys audio > "$audio_gate" || { rm -f "$audio_gate";return 1; }
 if awk '
 /- Actual mode = / {modes++;if($0!~/^[[:space:]]*- Actual mode = MODE_NORMAL[[:space:]]*$/)bad=1}
 /^## RecordActivityMonitor[[:space:]]*$/ {if(start++ || ended)bad=1;inside=1;next}
 /^### Recording Activity/ {if(!inside || ended++)bad=1;if(known&&!details)bad=1;inside=0;next}
 inside {
  if(/^[[:space:]]*$/)next
  if(/active[?: ]+true|active=true/){bad=1;next}
  if(/riid/){if(known&&!details)bad=1;known=0;details=0;if(/active\? false/){known=1;if(/AudioRecordingConfiguration|session:/)details=1}else bad=1;next}
  if(/AudioRecordingConfiguration|session:/){if(!known)bad=1;details=1;next}
  bad=1
 }
 END {exit bad || modes!=1 || start!=1 || ended!=1 || inside}' "$audio_gate";then gate_rc=0;else gate_rc=1;fi
 rm -f "$audio_gate" || return 1
 [ "$gate_rc" = 0 ] || { echo 'Refused: audio/capture state active or unverifiable' >&2;return 1; }
}
config_text(){ cat /data/adb/clarifae/config.conf; }
get_value(){ printf '%s\n' "$selection_config" | awk -F= -v k="$1" '$1==k{v=substr($0,length(k)+2);seen=1}END{if(!seen)exit 2;printf "%s",v}'; }
selected(){
 selection_config=$(config_text) || return 2
 raw=$(get_value out_targets) || return 2
 case "$raw" in ,*|*,|*,,*)echo 'Invalid output selection' >&2;return 2;;esac
 csv=
 oldifs=$IFS;IFS=,
 for token in $raw;do
  case "$token" in media|music)token=music;;calls|voice_call)token=voice_call;;notif|notification)token=notification;;ring|alarm|system);;*)echo 'Invalid output stream' >&2;return 2;;esac
  csv=$csv,$token
 done
 IFS=$oldifs
 result=
 for token in music voice_call ring alarm notification system;do case "$csv," in *,$token,*)[ -z "$result" ] || result=$result,;result=$result$token;;esac;done
 master=$(get_value enabled) && side=$(get_value out_enabled) || return 2
 case "$master:$side" in 1:1);;0:0|0:1|1:0)result=;;*)return 2;;esac
 printf '%s' "$result"
}
reload(){
 before=$(timeout 2 pidof android.hardware.audio.service_64 | cut -d' ' -f1)
 positive_id "$before" || return 1
 mutate setprop ctl.restart vendor.audio-hal || return 1
 n=0
 while [ "$n" -lt 18 ];do
  after=$(timeout 2 pidof android.hardware.audio.service_64 | cut -d' ' -f1)
  if [ -n "$after" ] && [ "$after" != "$before" ] && services;then echo "Audio services ready; HAL $before -> $after";return 0;fi
  sleep 1;n=$((n+1))
 done
 echo 'Audio service recovery timeout' >&2;return 1
}
restore(){
 T=$1;OLD=$(cat "$T/old") && managed=$(cat "$T/old-managed") || return 1
 snapshot_valid "$OLD" || return 1
 case "$managed" in 0|1);;*)return 1;;esac
 runtime_environment || return 1
 while IFS='|' read -r name path base;do
  top=$(mid "$path");baseid=$(cat "$R/base/mount-$name");oldid=$(cat "$T/mount-old-$name");newid=$(cat "$T/mount-new-$name" 2>/dev/null || true)
  positive_id "$top" && positive_id "$baseid" && positive_id "$oldid" || return 1
  case "$top" in "$baseid")need "$path" "$base" || return 1;;"$oldid")need "$path" "$(cat "$OLD/hash-$name")" || return 1;;"$newid")need "$path" "$(cat "$T/new/hash-$name")" || return 1;;*)echo "Rollback refused unowned mount: $path" >&2;return 1;;esac
  if [ "$top" != "$baseid" ];then
   atomic_text "$T/intent-restore-unmount-$name" "$top|$(file_sha "$path")" && mutate umount "$path" || return 1
  fi
  [ "$(mid "$path")" = "$baseid" ] && need "$path" "$base" || return 1
  if [ "$managed" = 1 ];then
   need "$OLD/$name.xml" "$(cat "$OLD/hash-$name")" || return 1
   atomic_text "$T/intent-restore-bind-$name" "$baseid|$(cat "$OLD/hash-$name")|$OLD/$name.xml" || return 1
   mutate mount --bind "$OLD/$name.xml" "$path" || return 1
   restored_id=$(mid "$path") && atomic_text "$T/mount-restored-$name" "$restored_id" || return 1
   mutate mount -o private none "$path" || return 1
  fi
  restored_id=$(mid "$path") && atomic_text "$R/state/mount-$name" "$restored_id" || return 1
 done < "$R/files"
 atomic_text "$R/state/current" "$OLD" && atomic_text "$R/state/managed" "$managed" || return 1
 if idle;then reload || return 1;echo restored > "$T/result" || return 1;else echo restored-reload-deferred > "$T/result" || return 1;fi
}
valid_transaction(){
 case "$1" in "$R/state/tx-"*);;*)return 1;;esac
 case "${1#"$R/state/tx-"}" in ''|*[!A-Za-z0-9-]*)return 1;;esac
 [ "$(readlink -f "$1")" = "$1" ] && [ -d "$1" ] && protected "$1"
}
mutation_lock(){
 valid_transaction "$1" || return 1
 mkdir "$1/mutation" 2>/dev/null || return 1
 echo $$ > "$1/mutation/pid" || return 1
}
mutation_unlock(){
 [ "$(cat "$1/mutation/pid" 2>/dev/null || true)" = "$$" ] || return 1
 rm "$1/mutation/pid" && rmdir "$1/mutation"
}
release_lock(){
 valid_transaction "$1" || return 1
 case "$(cat "$1/decision/result" 2>/dev/null || true)" in
  commit)[ "$(cat "$1/guard-terminal" 2>/dev/null || true)" = commit ] || return 1;;
  rollback)case "$(cat "$1/result" 2>/dev/null || true)" in restored|restored-reload-deferred|no-mutation);;*)return 1;;esac;;
  *)return 1;;
 esac
 mkdir "$1/release-owner" 2>/dev/null || return 0
 [ "$(cat "$R/lock/transaction" 2>/dev/null || true)" = "$1" ] || return 1
 rm -f "$R/lock/pid" "$R/lock/transaction" && rmdir "$R/lock"
}
mutate(){
 # Keep an explicit child pin: a TERM-interrupted shell must not unlock
 # recovery while a mount/reload command is still running independently.
 timeout 8 "$@" & MUTATOR_PID=$!
 if wait "$MUTATOR_PID";then mutator_rc=0;else mutator_rc=$?;fi
 if [ "$mutator_rc" -ge 124 ];then MUTATOR_UNCERTAIN=1;return 1;fi
 if kill -0 "$MUTATOR_PID" 2>/dev/null;then return 1;fi
 MUTATOR_PID=
 return "$mutator_rc"
}
recover(){
 T=$1
 valid_transaction "$T" || return 1
 mkdir "$T/decision" 2>/dev/null || return 2
 # The decision prevents new forward work. The mutex also waits for an
 # already-started mount/reload to quiesce before touching the same targets.
 # Never steal a held mutex: a killed/stuck owner requires audited recovery.
 wait=0
 until mutation_lock "$T";do
  if [ "$wait" -ge 8 ];then echo rollback-blocked > "$T/decision/result";return 1;fi
  sleep 1;wait=$((wait+1))
 done
 if restore "$T";then result=rollback;else result=rollback-blocked;fi
 if [ "${MUTATOR_UNCERTAIN:-0}" = 1 ] || { [ -n "${MUTATOR_PID:-}" ] && kill -0 "$MUTATOR_PID" 2>/dev/null; };then result=rollback-blocked;else mutation_unlock "$T" || result=rollback-blocked;fi
 echo "$result" > "$T/decision/result" || return 1
 [ "$result" = rollback ]
}
commit_verified(){
 [ "$(cat "$1/decision/result" 2>/dev/null || true)" = commit ] || return 1
 [ "$(cat "$1/result" 2>/dev/null || true)" = applied ] || return 1
 [ "$(current)" = "$(readlink -f "$1/new")" ] || return 1
 runtime && verify_xml && services
}
watch(){
 T=$1;valid_transaction "$T" || return 1
 atomic_text "$T/guard-ready" "$$" || return 1
 n=0
 while [ "$n" -lt 75 ];do
  terminal=$(cat "$T/decision/result" 2>/dev/null || true)
  case "$terminal" in
   commit)if commit_verified "$T";then atomic_text "$T/guard-terminal" commit;return;else atomic_text "$T/guard-terminal" blocked;return 1;fi;;
   rollback)atomic_text "$T/guard-terminal" rollback;return;;
   rollback-blocked)atomic_text "$T/guard-terminal" blocked;return 1;;
  esac
  [ ! -f "$T/abort" ] || { [ -d "$T/decision" ] || break; }
  sleep 1;n=$((n+1))
 done
 if [ -d "$T/decision" ];then atomic_text "$T/guard-terminal" blocked;return 1;fi
 if recover "$T";then atomic_text "$T/guard-terminal" rollback;release_lock "$T";else atomic_text "$T/guard-terminal" blocked;return 1;fi
}
start_guard(){
 toybox setsid toybox nohup sh "$0" watch "$T" > "$T/watch.log" 2>&1 < /dev/null &
 watch_pid=$!;atomic_text "$T/watch-pid" "$watch_pid" || return 1
 ready_wait=0
 while [ ! -f "$T/guard-ready" ];do
  kill -0 "$watch_pid" 2>/dev/null || return 1
  [ "$ready_wait" -lt 4 ] || return 1
  sleep 1;ready_wait=$((ready_wait+1))
 done
 positive_id "$(cat "$T/guard-ready")" && kill -0 "$(cat "$T/guard-ready")" 2>/dev/null
}
await_guard_commit(){
 ack_wait=0
 while [ ! -f "$T/guard-terminal" ];do [ "$ack_wait" -lt 4 ] || return 1;sleep 1;ack_wait=$((ack_wait+1));done
 [ "$(cat "$T/guard-terminal")" = commit ]
}
run(){
 mode=$1
 mkdir "$R/lock" 2>/dev/null || { echo 'Another routing operation is running' >&2;return 2; }
 echo $$ > "$R/lock/pid"
 T=;NEW=;MUTATOR_PID=;MUTATOR_UNCERTAIN=0;FORWARD_STARTED=0;done=0
 cleanup(){
  rc=$?;trap - EXIT INT TERM
  if [ -n "$T" ];then
   if [ "$MUTATOR_UNCERTAIN" = 1 ] || { [ -n "$MUTATOR_PID" ] && kill -0 "$MUTATOR_PID" 2>/dev/null; };then
    echo 'Mutating child live or completion unknown; recovery mutex retained' >&2
    touch "$T/abort"
   else
    mutation_unlock "$T" 2>/dev/null || true
    if [ "$done" != 1 ];then
     touch "$T/abort"
     if [ "$FORWARD_STARTED" = 0 ];then
      if mkdir "$T/decision" 2>/dev/null;then atomic_text "$T/result" no-mutation && atomic_text "$T/decision/result" rollback || true;fi
     else recover "$T" || true;fi
    fi
   fi
  fi
  # A render-only/no-op generation was never bound or journaled. Remove only
  # files this invocation created; retain all transaction generations/journals.
  if [ -z "$T" ] && [ -n "$NEW" ] && [ "$(current 2>/dev/null || true)" != "$NEW" ] && snapshot_path_valid "$NEW";then
   while IFS='|' read -r name path base;do rm -f "$NEW/$name.xml" "$NEW/hash-$name";done < "$R/files"
   rm -f "$NEW/targets";rmdir "$NEW" 2>/dev/null || true
  fi
  if [ -z "$T" ];then
   # No transaction/watcher was ever created: only this preflight owner exists.
   if [ "$(cat "$R/lock/pid" 2>/dev/null || true)" = "$$" ] && [ ! -f "$R/lock/transaction" ];then rm -f "$R/lock/pid";rmdir "$R/lock" 2>/dev/null || true;fi
  elif [ "$done" = 1 ] || [ "$(cat "$T/decision/result" 2>/dev/null || true)" = rollback ];then
   release_lock "$T" || true
  else echo "Recovery pending/blocked; lock and journal retained: $T" >&2;fi
  exit "$rc"
 }
 trap cleanup EXIT
 trap 'exit 124' INT TERM
 runtime;verify_xml;services
 targets=$(selected)
 NEW=$(mktemp -d "$RAM/g-XXXXXXXX")
 printf '%s\n' "$targets" > "$NEW/targets"
 same=1;OLD=$(current)
 while IFS='|' read -r name path base;do
  need "$R/base/$name.xml" "$base"
  awk -v targets="$targets" -f "$R/render-targets.awk" "$R/base/$name.xml" > "$NEW/$name.xml"
  [ -s "$NEW/$name.xml" ];chmod 0444 "$NEW/$name.xml";chcon "$(cat "$R/base/context-$name")" "$NEW/$name.xml"
  file_sha "$NEW/$name.xml" > "$NEW/hash-$name"
  [ "$(cat "$NEW/hash-$name")" = "$(cat "$OLD/hash-$name")" ] || same=0
 done < "$R/files"
 if [ "$same" = 1 ] && [ "$mode" = apply ];then done=1;echo "Playback routes already applied: $targets";return 0;fi
 idle
 T=$(mktemp -d "$R/state/tx-XXXXXXXX")
 echo "$T" > "$R/lock/transaction"
 printf '%s\n' "$OLD" > "$T/old";cp "$R/state/managed" "$T/old-managed"
 ln -s "$NEW" "$T/new"
 while IFS='|' read -r name path base;do cp "$R/state/mount-$name" "$T/mount-old-$name";done < "$R/files"
 start_guard
 mutation_lock "$T"
 FORWARD_STARTED=1
 managed=$(cat "$R/state/managed")
 while IFS='|' read -r name path base;do
  [ ! -d "$T/decision" ]
  [ "$(mid "$path")" = "$(cat "$T/mount-old-$name")" ]
  if [ "$managed" = 1 ];then atomic_text "$T/intent-unmount-$name" "$(mid "$path")|$(cat "$OLD/hash-$name")";mutate umount "$path";fi
  [ "$(mid "$path")" = "$(cat "$R/base/mount-$name")" ];need "$path" "$base"
  atomic_text "$T/intent-bind-$name" "$(mid "$path")|$(cat "$NEW/hash-$name")|$NEW/$name.xml"
  if ! mutate mount --bind "$NEW/$name.xml" "$path";then mid "$path" > "$T/observed-failed-bind-$name" 2>/dev/null || true;return 1;fi
  new_id=$(mid "$path");positive_id "$new_id";atomic_text "$T/mount-new-$name" "$new_id"
  mutate mount -o private none "$path"
  need "$path" "$(cat "$NEW/hash-$name")"
 done < "$R/files"
 idle
 reload
 runtime;services
 [ "$(selected)" = "$targets" ] || { echo 'Settings changed during Apply; retry newest selection' >&2;return 2; }
 while IFS='|' read -r name path base;do need "$path" "$(cat "$NEW/hash-$name")";[ "$(mid "$path")" = "$(cat "$T/mount-new-$name")" ];done < "$R/files"
 mkdir "$T/decision" || return 2
 commit_state(){
  atomic_text "$R/state/current" "$NEW" && atomic_text "$R/state/managed" 1 || return 1
  while IFS='|' read -r name path base;do atomic_text "$R/state/mount-$name" "$(cat "$T/mount-new-$name")" || return 1;done < "$R/files"
  atomic_text "$T/result" applied && atomic_text "$T/decision/result" commit
 }
 if ! commit_state;then rmdir "$T/decision" 2>/dev/null || true;return 1;fi
 await_guard_commit
 done=1
 echo "Applied playback routes without reboot: $targets"
}
[ $# -gt 0 ] || set -- help
case "$1" in
 apply|restart)CLARIFAE_BOUNDED_WORKER=1;export CLARIFAE_BOUNDED_WORKER;exec timeout 45 sh "$0" _worker "$1";;
 _worker)[ "${CLARIFAE_BOUNDED_WORKER:-}" = 1 ] || exit 2;unset CLARIFAE_BOUNDED_WORKER;case "${2:-}" in apply|restart)run "$2";;*)exit 2;;esac;;
 watch)valid_transaction "${2:-}" && watch "$2";;
 verify)[ ! -d "$R/lock" ] || { echo 'Routing transaction/recovery pending' >&2;exit 2; };runtime;verify_xml;services;echo "Live routing verified: $(cat "$(current)/targets")";;
 *)echo 'Usage: apply|restart|verify' >&2;exit 2;;
esac
