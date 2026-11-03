#!/system/bin/sh
# functions.sh — shared Clarifae helpers, sourced by customize.sh (install time)
# and by clarifae-ctl (runtime, from /data/adb/clarifae/functions.sh).
# POSIX sh / busybox ash only — no bashisms.

CLARIFAE_DIR=/data/adb/clarifae
CONFIG="$CLARIFAE_DIR/config.conf"
LOGFILE="$CLARIFAE_DIR/clarifae.log"
PATCH_MARK="$CLARIFAE_DIR/audio_effects.patched"   # holds the patched file path
BACKUP="$CLARIFAE_DIR/audio_effects.backup"
MODULE_DIR=/data/adb/modules/clarifae              # always present post-boot, mount-namespace independent

CLARIFAE_VERSION="v1.1.2"
EFFECT_UUID="c1a71fae-0001-4a5e-9b10-43578768aaf3"       # input  (capture pre-proc)
EFFECT_UUID_OUT="c1a71fae-0002-4a5e-9b10-43578768aaf3"   # output (playback post-proc)
LIB_NAME="clarifae"
EFFECT_NAME="clarifae_ns"
OUT_EFFECT_NAME="clarifae_out"

CLARIFAE_KEYS="enabled in_enabled out_enabled mode strength out_strength atten_db vad targets out_targets highpass autogain loglevel"

clarifae_defaults() {
  cat <<'EOF'
enabled=1
in_enabled=1
out_enabled=1
mode=1
strength=70
out_strength=60
atten_db=24
vad=50
targets=mic,voice_comm
out_targets=music,voice_call
highpass=1
autogain=0
loglevel=1
EOF
}

clarifae_log() {
  mkdir -p "$CLARIFAE_DIR" 2>/dev/null
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOGFILE" 2>/dev/null
}

conf_init() {
  mkdir -p "$CLARIFAE_DIR" 2>/dev/null || return 1
  [ -f "$CONFIG" ] || clarifae_defaults > "$CONFIG"
}

conf_key_valid() {
  case "$1" in
    enabled|in_enabled|out_enabled|mode|strength|out_strength|atten_db|vad|targets|out_targets|highpass|autogain|loglevel) return 0 ;;
    *) return 1 ;;
  esac
}

# conf_get KEY -> value. Only a missing key falls back; an explicit empty stays empty.
conf_get() (
  [ "$#" -eq 1 ] || return 1
  conf_key_valid "$1" || return 1
  _get_key="$1"
  if [ -f "$CONFIG" ]; then
    _get_line=$(grep "^$_get_key=" "$CONFIG")
    _get_rc=$?
    case "$_get_rc" in
      0) _get_line=$(printf '%s\n' "$_get_line" | tail -n1)
         printf '%s' "${_get_line#*=}"; return 0 ;;
      1) ;; # absent, not empty
      *) return 1 ;;
    esac
  elif [ -e "$CONFIG" ]; then
    return 1
  fi
  _get_line=$(clarifae_defaults | grep "^$_get_key=") || return 1
  printf '%s' "${_get_line#*=}"
)

# conf_set KEY VALUE. Do not interpolate values into a sed/awk program.
# Config values occupy one line; reject line injection even for direct helper users.
conf_set() (
  [ "$#" -eq 2 ] || return 1
  conf_key_valid "$1" || return 1
  _set_key="$1"; _set_value="$2"
  _set_cr=$(printf '\r')
  case "$_set_value" in *'
'*|*"$_set_cr"*) return 1 ;; esac
  conf_init || return 1
  _set_tmp="$CONFIG.tmp.$$"
  trap 'rm -f "$_set_tmp"' 0
  CLARIFAE_CONF_KEY="$_set_key" CLARIFAE_CONF_VALUE="$_set_value" awk '
    BEGIN { key=ENVIRON["CLARIFAE_CONF_KEY"]; value=ENVIRON["CLARIFAE_CONF_VALUE"]; found=0 }
    index($0, key "=")==1 { if (!found) print key "=" value; found=1; next }
    { print }
    END { if (!found) print key "=" value }
  ' "$CONFIG" > "$_set_tmp" && mv "$_set_tmp" "$CONFIG"
)

# write a single property (persisted)
_setp() {
  if command -v resetprop >/dev/null 2>&1; then
    resetprop -n -p "$1" "$2"
  else
    setprop "$1" "$2"
  fi
}

# Push every config key; any failed read/write makes the whole sync unsuccessful.
prop_sync() {
  _sync_rc=0
  for _sync_key in $CLARIFAE_KEYS; do
    if _sync_value=$(conf_get "$_sync_key"); then
      _setp "persist.clarifae.$_sync_key" "$_sync_value" || _sync_rc=1
    else
      _sync_rc=1
    fi
  done
  clarifae_log "props synced from config (rc=$_sync_rc)"
  return "$_sync_rc"
}

mode_name() {
  case "$1" in
    1) echo "RNNoise (Low latency)";;
    2) echo "DTLN (Balanced)";;
    3) echo "DeepFilterNet (Maximum clarity)";;
    *) echo "Unknown";;
  esac
}

# ---- audio_effects.xml / .conf integration ----

find_audio_effects_file() {
  for _f in /vendor/etc/audio_effects.xml /odm/etc/audio_effects.xml \
            /system/etc/audio_effects.xml /vendor/etc/audio_effects.conf \
            /system/etc/audio_effects.conf; do
    [ -f "$_f" ] && { echo "$_f"; return 0; }
  done
  return 1
}

# map a real partition path to the module overlay path (Magisk magic-mount)
overlay_path_for() {
  _mp="$1"; _real="$2"
  case "$_real" in
    /vendor/*) echo "$_mp/system$_real" ;;          # system/vendor -> /vendor
    /odm/*)    echo "$_mp/system/odm${_real#/odm}" ;;
    /system/*) echo "$_mp$_real" ;;
    *)         echo "$_mp/system$_real" ;;
  esac
}

# our csv target names -> AOSP audio_source stream types (capture / <preprocess>)
map_targets() {
  _out=""
  _oifs="$IFS"; IFS=,
  for _t in $1; do
    case "$_t" in
      mic)        _s=mic ;;
      voice_comm) _s=voice_communication ;;
      camcorder)  _s=camcorder ;;
      voice_rec)  _s=voice_recognition ;;
      *)          _s="" ;;
    esac
    [ -n "$_s" ] && _out="$_out $_s"
  done
  IFS="$_oifs"
  echo "$_out"
}

# our csv out target names -> AOSP audio_stream_type names (playback / <postprocess>)
map_out_targets() {
  _out=""
  _oifs="$IFS"; IFS=,
  for _t in $1; do
    case "$_t" in
      media|music)       _s=music ;;
      calls|voice_call)  _s=voice_call ;;
      ring)              _s=ring ;;
      alarm)             _s=alarm ;;
      notif|notification) _s=notification ;;
      system)            _s=system ;;
      *)                 _s="" ;;
    esac
    [ -n "$_s" ] && _out="$_out $_s"
  done
  IFS="$_oifs"
  echo "$_out"
}

_patch_xml_file() {
  _src="$1"; _dst="$2"; _in_streams="$3"; _out_streams="$4"
  awk -v uuid_in="$EFFECT_UUID" -v uuid_out="$EFFECT_UUID_OUT" -v lib="$LIB_NAME" \
      -v eff_in="$EFFECT_NAME" -v eff_out="$OUT_EFFECT_NAME" \
      -v in_streams="$_in_streams" -v out_streams="$_out_streams" '
    BEGIN {
      lib_done=0; eff_done=0; pre_done=0; post_done=0; section=""; cur="";
      ni=split(in_streams,  wi, " "); for (i=1;i<=ni;i++) if (wi[i]!="") hin[wi[i]]=0;
      no=split(out_streams, wo, " "); for (i=1;i<=no;i++) if (wo[i]!="") hout[wo[i]]=0;
    }
    function emit_stream(t, e) {
      print "        <stream type=\"" t "\">";
      print "            <apply effect=\"" e "\"/>";
      print "        </stream>";
    }
    function emit_pre_unhandled(   i)  { for (i=1;i<=ni;i++) if (wi[i]!="" && hin[wi[i]]==0)  { emit_stream(wi[i], eff_in);  hin[wi[i]]=1 } }
    function emit_post_unhandled(  i)  { for (i=1;i<=no;i++) if (wo[i]!="" && hout[wo[i]]==0) { emit_stream(wo[i], eff_out); hout[wo[i]]=1 } }
    /<\/libraries>/ && !lib_done {
      print "        <library name=\"" lib "\" path=\"libclarifae.so\"/>"; lib_done=1
    }
    /<\/effects>/ && !eff_done {
      print "        <effect name=\"" eff_in  "\" library=\"" lib "\" uuid=\"" uuid_in  "\"/>";
      print "        <effect name=\"" eff_out "\" library=\"" lib "\" uuid=\"" uuid_out "\"/>";
      eff_done=1
    }
    /<preprocess>/  { section="pre" }
    /<postprocess>/ { section="post" }
    {
      if (section!="" && /<stream/ && match($0, /type="[^"]*"/))
        cur = substr($0, RSTART + 6, RLENGTH - 7);
    }
    /<\/stream>/ {
      if (cur != "") {
        if (section=="pre"  && (cur in hin)  && hin[cur]==0)  { print "            <apply effect=\"" eff_in  "\"/>"; hin[cur]=1 }
        if (section=="post" && (cur in hout) && hout[cur]==0) { print "            <apply effect=\"" eff_out "\"/>"; hout[cur]=1 }
      }
      cur="";
    }
    /<\/preprocess>/  { emit_pre_unhandled();  pre_done=1;  section="" }
    /<\/postprocess>/ { emit_post_unhandled(); post_done=1; section="" }
    /<\/audio_effects_conf>/ {
      if (!pre_done)  { print "    <preprocess>";  emit_pre_unhandled();  print "    </preprocess>";  pre_done=1 }
      if (!post_done) { print "    <postprocess>"; emit_post_unhandled(); print "    </postprocess>"; post_done=1 }
    }
    { print }
  ' "$_src" > "$_dst" 2>/dev/null
  [ -s "$_dst" ] && grep -q "clarifae" "$_dst"
}

_patch_conf_file() {
  _src="$1"; _dst="$2"; _in_streams="$3"; _out_streams="$4"
  awk -v uuid_in="$EFFECT_UUID" -v uuid_out="$EFFECT_UUID_OUT" -v lib="$LIB_NAME" \
      -v eff_in="$EFFECT_NAME" -v eff_out="$OUT_EFFECT_NAME" \
      -v in_streams="$_in_streams" -v out_streams="$_out_streams" '
    BEGIN { lib_done=0; eff_done=0 }
    { print }
    /libraries[ \t]*\{/ && !lib_done { print "  " lib " { path libclarifae.so }"; lib_done=1 }
    /effects[ \t]*\{/   && !eff_done {
      print "  " eff_in  " { library " lib " uuid " uuid_in  " }";
      print "  " eff_out " { library " lib " uuid " uuid_out " }";
      eff_done=1
    }
    /pre_processing[ \t]*\{/ {
      n=split(in_streams, ai, " ");
      for (i=1;i<=n;i++) if (ai[i]!="") print "  " ai[i] " { " eff_in " {} }";
    }
    /post_processing[ \t]*\{/ {
      n=split(out_streams, ao, " ");
      for (i=1;i<=n;i++) if (ao[i]!="") print "  " ao[i] " { " eff_out " {} }";
    }
  ' "$_src" > "$_dst" 2>/dev/null
  [ -s "$_dst" ] && grep -q "clarifae" "$_dst"
}

# patch_audio_effects MODPATH -> 0 ok / 1 fail. Writes overlay + backup + marker.
patch_audio_effects() {
  _mp="$1"
  _real="$(find_audio_effects_file)" || { clarifae_log "audio_effects file not found"; return 1; }
  _streams="$(map_targets "$(conf_get targets)")"
  _out_streams="$(map_out_targets "$(conf_get out_targets)")"
  _overlay="$(overlay_path_for "$_mp" "$_real")"

  if grep -q "clarifae" "$_real" 2>/dev/null; then
    clarifae_log "audio_effects already references clarifae ($_real)"
    echo "$_real" > "$PATCH_MARK"
    return 0
  fi

  mkdir -p "$(dirname "$_overlay")" 2>/dev/null
  cp -f "$_real" "$BACKUP" 2>/dev/null

  case "$_real" in
    *.xml)  _patch_xml_file  "$_real" "$_overlay" "$_streams" "$_out_streams" ;;
    *.conf) _patch_conf_file "$_real" "$_overlay" "$_streams" "$_out_streams" ;;
    *) return 1 ;;
  esac

  if [ $? -eq 0 ]; then
    chmod 0644 "$_overlay" 2>/dev/null
    echo "$_overlay" > "$PATCH_MARK"
    clarifae_log "patched audio_effects: $_real -> $_overlay (in:$_streams | out:$_out_streams)"
    return 0
  fi
  clarifae_log "FAILED to patch $_real"
  return 1
}

is_audio_effects_patched() {
  _f="$(find_audio_effects_file)" || return 1
  # Mounted (real) location — only reflects the patch where the systemless
  # overlay is applied. The WebUI's exec namespace may not see it.
  grep -q "clarifae" "$_f" 2>/dev/null && return 0
  # Namespace-independent fallback: the staged overlay in the module dir.
  _ov="$(overlay_path_for "$MODULE_DIR" "$_f")"
  [ -f "$_ov" ] && grep -q "clarifae" "$_ov" 2>/dev/null
}
