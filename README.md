<div align="center">
  <img src="webroot/assets/logo.svg" alt="Clarifae" height="64" />
  <h3>Real-time AI audio clarity for rooted Android</h3>
  <p>A Magisk / KernelSU / APatch module that cleans both your <b>microphone</b>
  and the <b>audio you hear</b> in real time using open-source speech-enhancement
  models — configurable from a WebUI.</p>
</div>

---

Clarifae installs one native effect library (`libclarifae.so`) that exposes **two
descriptors**, registered via `audio_effects.xml`:

- a **capture pre-processor** on input streams (microphone, calls/VoIP, camcorder,
  voice recognition) — cleans *your* outgoing voice; and
- a **playback post-processor** on output streams (media/music, calls, …) — cleans
  the audio *you hear*.

Each side can be toggled independently and is denoised on the fly by one of three
engines, selectable as **clarity modes**.

> **Note on output + music:** these are *speech-enhancement* models. On voice content
> (calls, videos, podcasts) output cleanup clarifies nicely; on actual **music** it
> treats instruments as noise and degrades them — use the separate *Playback strength*
> slider (or turn output off) to taste.

> Brand: primary `#435787`, secondary `#68aaf3`, font *Creative Caps*.

## Contents

- [Clarity modes](#clarity-modes)
- [Features](#features)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [How it works](#how-it-works)
- [Configuration reference](#configuration-reference)
- [`clarifae-ctl` commands](#clarifae-ctl-commands)
- [Limitations & caveats](#limitations--caveats)
- [Licensing & credits](#licensing--credits)

## Clarity modes

| Mode | Engine | Profile | Status |
|---|---|---|---|
| **1 · Low latency** | [RNNoise](https://github.com/xiph/rnnoise) | Compact RNN denoiser, lowest CPU/latency, 48 kHz framed | ✅ **Default, works out of the box** |
| **2 · Balanced** | [DTLN](https://github.com/breizhn/DTLN) | Dual-signal LSTM via TensorFlow Lite, stronger denoising | ⚠️ Experimental — needs TFLite + models |
| **3 · Maximum clarity** | [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) | Full-band 48 kHz deep filtering, highest quality | ⚠️ Experimental — needs `libdf` + model |

Modes 2 and 3 are optional builds. If their runtime/models aren't present, Clarifae
**transparently falls back to RNNoise** — it never silently breaks your audio.

## Features

- **Both directions:** mic input *and* playback output cleanup, each toggleable, from
  one native AudioFlinger effect (pre-processing + post-processing descriptors).
- Three AI clarity modes with graceful fallback.
- **Branded WebUI**: master + per-side toggles, mode picker, separate mic/playback
  strength, max-suppression / voice-gate sliders, high-pass & auto-gain, capture-source
  and playback-stream selection, live engine status, logs.
- Live, reboot-free settings via `persist.clarifae.*` properties (the effect re-reads
  on change).
- `clarifae-ctl` CLI for scripting and debugging.
- Clean uninstall — the original `audio_effects.xml` is only *shadowed*, never edited.

## Requirements

- A rooted Android device with **Magisk ≥ 24.0**, **KernelSU**, or **APatch**.
- For the WebUI: KernelSU/APatch have a built-in WebUI launcher; on **Magisk**, install
  **[MMRL](https://github.com/MMRLApp/MMRL)** to open module WebUIs.
- To build from source: Android NDK (r27+), CMake, and network access for `fetch_deps.sh`.

## Quick start

```bash
# from the project folder:
# 1. fetch RNNoise + weights, then build the native effect for all ABIs
./scripts/fetch_deps.sh
./build.sh                      # -> lib/<abi>/

# 2. package a flashable zip
./scripts/package.sh            # -> dist/clarifae-v1.0.0.zip

# 3. flash dist/clarifae-v1.0.0.zip in Magisk/KernelSU/APatch, reboot
# 4. open the Clarifae WebUI, toggle it on, pick a mode
```

See **[INSTALL.md](INSTALL.md)** for details, optional DTLN/DeepFilterNet builds, and
verification steps.

## How it works

**Registration, attachment and PCM processing are separate gates.** A registered
or attached effect does not prove that a particular vendor path sends it audio.
See [legacy ABI/streaming tests and limitations](docs/LEGACY_EFFECT_TESTING.md);
these portable fixes are not a universal Samsung capture fix.

```
 INPUT   mic app ─▶ AudioRecord ─▶ capture chain  ─┐  <preprocess> → clarifae_ns
                                                    ├─▶ libclarifae.so (effect.c)
 OUTPUT  media/call ◀─ AudioTrack ◀─ playback mix ─┘  <postprocess> → clarifae_out
                               │  s16/float ⇄ float, live config from persist.clarifae.*
                               ▼
                  backend: rnnoise │ dtln │ deepfilternet
                               │  resample ⇄ engine rate, frame, enhance, wet/dry + atten cap
                               ▼
              cleaned audio ─▶ back to the app (input) / to the speaker (output)
```

Configuration flows: **WebUI → `clarifae-ctl` → `config.conf` → `persist.clarifae.*` →
effect**. Each instance detects property identity/serial/value changes and reloads
parameters / re-opens the backend on the next audio buffer, so most changes apply without
a reboot. See **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

## Configuration reference

`/data/adb/clarifae/config.conf` is the source of truth (edited by the WebUI/`clarifae-ctl`)
and is mirrored to system properties the effect reads:

| Key / property `persist.clarifae.*` | Values | Default | Meaning |
|---|---|---|---|
| `enabled`      | `0` / `1` | `1` | Master on/off (bypass = passthrough) |
| `in_enabled`   | `0` / `1` | `1` | Clean the **microphone** (input) |
| `out_enabled`  | `0` / `1` | `1` | Clean **playback** (output) |
| `mode`         | `1` / `2` / `3` | `1` | RNNoise / DTLN / DeepFilterNet |
| `strength`     | `0`–`100` | `70` | Wet/dry blend for the **mic** |
| `out_strength` | `0`–`100` | `60` | Wet/dry blend for **playback** (lower for music) |
| `atten_db`     | `0`–`60` | `24` | Max suppression depth (protects tone) |
| `vad`          | `0`–`100` | `50` | Voice-activity sensitivity (gate / auto-gain) |
| `targets`      | csv of `mic,voice_comm,camcorder,voice_rec` | `mic,voice_comm` | Capture sources (input) |
| `out_targets`  | csv of `music,voice_call,ring,alarm,notification,system` | `music,voice_call` | Playback streams (output) |
| `highpass`     | `0` / `1` | `1` | DC/rumble high-pass pre-filter |
| `autogain`     | `0` / `1` | `0` | Gentle make-up gain on speech |
| `loglevel`     | `0`–`3` | `1` | logcat verbosity (tag `Clarifae`) |

Effect UUIDs: input (pre-proc) `c1a71fae-0001-…` (type = AOSP noise-suppression);
output (post-proc) `c1a71fae-0002-…` (custom Clarifae type).

## `clarifae-ctl` commands

```
clarifae-ctl get                 # print config.conf
clarifae-ctl get-json            # settings as JSON
clarifae-ctl set KEY VALUE       # validate + write config + sync property
clarifae-ctl set-many K=V K=V…   # batch set
clarifae-ctl apply               # sync settings only; installed XML routes unchanged
clarifae-ctl status              # human-readable engine status
clarifae-ctl status-json         # status as JSON (used by the WebUI)
clarifae-ctl restart-audio       # sync settings, request audioserver restart (interrupts audio)
clarifae-ctl reset               # restore defaults
clarifae-ctl log                 # tail ctl log + logcat -s Clarifae
clarifae-ctl version
```

### Settings saves, Apply and Restart

The WebUI quotes every shell argument (including an empty value), serializes
writes, and flushes debounced slider saves **before** Apply or Restart. A failed
save blocks either action until that setting is successfully saved again, or a
successful Reset restores defaults. CLI config-write, property-write and partial
property-sync failures return nonzero and are shown instead of a success toast.
Batch `set-many` and property sync are not transactions: an error can leave some
settings saved/synced; independent CLI processes are not serialized by the WebUI.

**Apply only syncs saved settings to `persist.clarifae.*`.** This portable module
has no live XML-route manager or capture HAL adapter. Changing the **installed
target list** still requires reinstalling/regenerating the route overlay and
activating it using the platform's normal procedure (typically re-flash + reboot).
Neither Apply nor Restart regenerates routes. An explicit
`clarifae-ctl set out_targets ''` remains empty in config, JSON and property sync;
it does not by itself remove already installed XML entries. Only a missing key
falls back to the unchanged default. Capture target lists must remain nonempty.
CSV tokens must match the existing per-side route maps (legacy output aliases
`media`, `calls`, `notif` are also accepted); malformed/unknown tokens,
whitespace and line injection are rejected.

Restart preserves the interruption warning: active recordings/calls **can glitch**,
and there is **no active-call protection**. After successful settings sync, the CLI
makes one checked `setprop ctl.restart audioserver` request, with no unconditional
kill/stop/start fallbacks. A zero return proves only that the request was accepted,
not that audio recovered, routes changed, or live PCM was processed. The WebUI says
“requested (not verified)”, never “restarted”. Status detection is heuristic;
registration, attachment and PCM processing still need independent verification.

UI commands wait up to 65 seconds; the bridge clamps caller timeouts to 8–70 seconds
and cleans callbacks/timers on completion, exception or timeout. Slow status polls
are coalesced and automatic polling pauses during saves/actions, so polls cannot
build a backlog ahead of saves. A mutation timeout,
ambiguous bridge exception or invalid completion code means **completion unknown**,
not host cancellation. The UI quarantines **all further mutations**, including
already queued saves, same-key retries, Reset and Restart, for that page session.
A persistent warning remains; read-only status/log queries do not unlock writes.
Verify that the original host command has actually finished and reconcile saved
settings before reopening the WebUI. Reopening alone does not cancel a command;
there is no automatic safe recovery in this portable bridge. Known nonzero command
exits remain retryable. Desktop preview remains a mock, not device evidence.

### Host-only control regression tests

~~~sh
node --test tests/test_ui.cjs
sh tests/test_control.sh
# Optional second POSIX shell implementation:
TEST_SH=/path/to/dash sh tests/test_control.sh
~~~

The JavaScript suite uses a mocked DOM/bridge and a local POSIX shell only for
argument roundtrips. The POSIX fixture isolates config, property and service
commands under ignored `build/`; it exercises empty/missing values, key/CSV
validation, config/property/sync errors and honest restart requests without device
access. No npm install, root access, hardware recording or route change is needed.
Run the existing ASan/UBSan native suites separately with `tests/run-host.sh`
and an external `RNNOISE_DIR` as described in
[legacy effect testing](docs/LEGACY_EFFECT_TESTING.md). Native/backend behavior,
installer defaults, SELinux policy, model selection and music routing are unchanged.

## Limitations & caveats

- **Auto-attach is device-dependent.** It edits the device's `audio_effects.xml` via an
  overlay. Some vendors use non-standard formats/locations; if patching fails the library is
  still installed and you can attach it manually (see
  [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)).
- **Not every app/stream gets cleaned.** Effects apply per *audio source* (input) and per
  *stream type* (output). Apps using their own DSP, a raw/`unprocessed` source, direct/offload
  output, or exotic routes may bypass it. New sessions pick up changes; some need a `restart-audio`.
- **Output post-processing support varies by device** more than input pre-processing — some
  ROMs ignore or restrict `<postprocess>` stream effects.
- **Music will be degraded** if you enable output on the `music` stream — these are speech
  models. Use *Playback strength* or disable playback cleanup for music.
- **SELinux/vendor variance.** `persist.clarifae.*` live under `default_prop`; a defensive
  `sepolicy.rule` is included for stricter devices.
- **DTLN/DeepFilterNet are experimental** and require extra runtimes/models; without them
  Clarifae uses RNNoise.
- With RNNoise v0.2, aligned streaming adds 30 ms at 48 kHz, plus platform latency;
  other rates include resampling/priming rounding. Full bypass is immediate.

## Licensing & credits

Clarifae module code is provided as-is for the brand owner. It builds on:

- **RNNoise** — BSD-3-Clause — <https://github.com/xiph/rnnoise>
- **DeepFilterNet** — MIT / Apache-2.0 — <https://github.com/Rikorose/DeepFilterNet>
- **DTLN** — see repo — <https://github.com/breizhn/DTLN> · web: <https://github.com/sapphi-red/DTLN-web>
- **RNNoise voice plugin** — <https://github.com/werman/noise-suppression-for-voice>
- **DTLN-AEC** (echo cancellation) — <https://github.com/breizhn/DTLN-aec>
- The vendored `native/include/audio_effect.h` is a trimmed AOSP-compatible ABI header
  (Apache-2.0, The Android Open Source Project).

Verify each upstream license before redistribution. TensorFlow Lite (DTLN backend) is
Apache-2.0.
