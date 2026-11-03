# Clarifae architecture

## Overview

Clarifae is a native **Android audio pre-processing effect** plus the glue to deploy and
configure it as a root module. It does not patch apps or the framework — it plugs into the
standard AudioFlinger capture-effect mechanism that already exists for AEC/NS/AGC.

## Data flow

```
 INPUT path (clean the mic):
   recording app ─ AudioRecord ─▶ capture chain ─ <preprocess> ─▶ clarifae_ns ─┐
        ▲ cleaned mic PCM ──────────────────────────────────────────────────── ┘

 OUTPUT path (clean what you hear):
   media/call ◀─ AudioTrack ◀─ playback mix ─ <postprocess> ─▶ clarifae_out ──┐
        speaker / headset ◀──────────────── cleaned PCM ──────────────────────┘

 Both descriptors live in ONE library and share the DSP:
        ┌──────────────────────────┐
        │  libclarifae.so           │
        │  effect.c (ABI: AELI sym) │  kind = INPUT | OUTPUT (set from UUID)
        │   • SET_CONFIG: rate/ch/fmt
        │   • process(): s16/float⇄f │  gated by in_enabled / out_enabled
        │   • live cfg via props     │  strength (in) / out_strength (out)
        └─────────────┬────────────┘
                       │ float [-32768,32767], interleaved
                       ▼
        ┌──────────────────────────┐
        │  backend (clarifae_backend_t)
        │  rnnoise │ dtln │ deepfilternet
        │   resample ⇄ engine rate, frame,
        │   enhance, wet/dry mix + atten cap
        └──────────────────────────┘
```

## The Android effect ABI (`native/effect.c`, `native/include/audio_effect.h`)

- The library exports `audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM` — the symbol
  **`AELI`** — which AudioFlinger's effect loader looks up via `dlsym`.
- It advertises **two descriptors** from the one library, dispatched by UUID in
  `create_effect`/`get_descriptor`:
  - **INPUT** (capture pre-proc): type = FX_IID_NS (`58b4b260-…`), uuid
    `c1a71fae-0001-…`, flags `EFFECT_FLAG_TYPE_PRE_PROC | INSERT_FIRST | INPUT_DIRECT | OUTPUT_DIRECT`.
  - **OUTPUT** (playback post-proc): custom type `c1a71fae-00c0-…`, uuid
    `c1a71fae-0002-…`, flags `EFFECT_FLAG_TYPE_POST_PROC | INSERT_LAST | INPUT_DIRECT | OUTPUT_DIRECT`.
  Each instance records its `kind` from the UUID; `process()` is identical DSP, gated by
  `in_enabled`/`out_enabled` and using `strength`/`out_strength` respectively.
- Per-instance state begins with `const effect_interface_s* itfe;` so the `effect_handle_t`
  AudioFlinger holds aliases the context pointer.
- `command()` handles `SET_CONFIG` (sample rate / channel mask / format / access mode),
  `GET_CONFIG`, `RESET`, `ENABLE/DISABLE`, and device/volume/source notifications.
  Unsupported parameter commands return an error instead of fabricated success. `process()` does format conversion (s16 ⇄ float, honoring
  `EFFECT_BUFFER_ACCESS_ACCUMULATE`) and calls the active backend.

The NDK ships no `hardware/audio_effect.h`, so a trimmed, layout-compatible copy is vendored
at `native/include/audio_effect.h`.

## Backend abstraction (`native/backend.h`)

```c
typedef struct clarifae_backend {
    const char* name;
    void* (*create)(int sample_rate, int channels, char* err, int errlen);
    int   (*process)(void* st, float* inout, int frames);   // in place
    void  (*set_params)(void* st, float strength, float atten_db, float vad,
                        int highpass, int autogain);
    void  (*reset)(void* st);
    void  (*destroy)(void* st);
} clarifae_backend_t;
```

`clarifae_backend_for_mode(mode)` returns the engine for the mode, with optional engines
declared `__attribute__((weak))` so they resolve to `NULL` when not linked. `effect.c`
calls the selected backend's `create()`; if an optional engine fails (missing model/runtime)
it falls back to RNNoise.

- **RNNoise** (`backend_rnnoise.c`, always built): resamples session ⇄ 48 kHz, buffers into
  480-sample frames, runs `rnnoise_process_frame`, blends wet/dry by `strength`, bounds
  suppression to `atten_db`, optional high-pass + auto-gain, resamples back. Per-channel
  state; fixed zero-priming and delay-aligned wet/dry mixing, preserving filter tails.
- **DTLN** (`backend_dtln.c`, `CLARIFAE_WITH_DTLN`): TFLite C API, 16 kHz, 512/128
  block/shift, two interpreters with carried LSTM state, overlap-add, self-contained FFT.
- **DeepFilterNet** (`backend_deepfilternet.c`, `CLARIFAE_WITH_DF`): calls `libdf`'s C-FFI
  (`df_create`/`df_process_frame`/…), full-band 48 kHz.

## Configuration flow

```
WebUI ──ksu.exec──▶ clarifae-ctl ──▶ /data/adb/clarifae/config.conf  (source of truth)
                              │
                              └──▶ resetprop -p persist.clarifae.<key>  (runtime channel)
                                                   │
                              effect.c: clarifae_config_changed(&instance_watch)  (exact snapshots)
                                                   │ on change
                              clarifae_config_load() → set_params / re-open backend
```

System **properties** are the live channel because SELinux forbids `audioserver` from reading
`/data/adb`. Each effect compares exact property identity/serial/value snapshots using
public API 26 `__system_property_read_callback` (O(keys)). The watcher runs before
configuration loading so a racing write cannot be consumed without a later reload.
Multi-property writes are not transactional. The shared log level is atomic. A mode
change selecting a different available backend re-opens it in the legacy process
path; heavy model/runtime setup is still a real-time risk. Ordinary parameter changes
call `set_params`. No vendor aliases or dynamic backend plugins are introduced.

## audio_effects.xml integration (`common/functions.sh`)

At install, `patch_audio_effects` locates the device file (`/vendor/etc`, `/odm/etc`, or
`/system/etc`; `.xml` or legacy `.conf`), copies it into the **module overlay** at the
matching path (Magisk magic-mounts `system/vendor → /vendor`, etc.), and idempotently injects:

- `<library name="clarifae" path="libclarifae.so"/>` in `<libraries>`,
- **two** effects in `<effects>`:
  - `<effect name="clarifae_ns"  library="clarifae" uuid="c1a71fae-0001-…"/>` (input)
  - `<effect name="clarifae_out" library="clarifae" uuid="c1a71fae-0002-…"/>` (output)
- `<apply effect="clarifae_ns"/>` into each capture `<stream type="…">` under `<preprocess>`
  (from `targets`), and `<apply effect="clarifae_out"/>` into each playback `<stream type="…">`
  under `<postprocess>` (from `out_targets`). In both blocks the injector appends to an existing
  matching stream, or creates the stream / the whole `<preprocess>`/`<postprocess>` block as needed.

Because it's an overlay, the original file is never modified — removing the module reverts it.

## Resampling & latency (`native/resampler.c`)

A continuous linear-interpolation resampler with one sample of carried history joins
consecutive `process()` calls. Scheduling is independent of callback partitioning.
For RNNoise v0.2 at 48 kHz, 480 samples of fixed priming plus 960 samples of native
lookahead gives 1440 samples / 30 ms, including the aligned dry branch. Other rates
add resampling/priming rounding. This linear resampler is not a high-quality
anti-aliasing filter; the figure excludes platform and hardware latency.

Registration, attachment and real PCM processing must be verified separately.
An attached legacy capture effect may be control-only on a vendor path. See
[ABI/streaming tests and limits](LEGACY_EFFECT_TESTING.md) for reproducible host
tests and what they cannot prove; this is not a universal Samsung fix.

## Module layout

The project folder is the module root (flat — no nested dir):

```
clarifae/  (project root == module root)
  module.prop  customize.sh  service.sh  post-fs-data.sh  uninstall.sh
  system.prop  sepolicy.rule  META-INF/.../update-binary
  common/   functions.sh  clarifae-ctl  audio_effects.clarifae.xml
  native/   effect.c config.c ringbuf.c resampler.c backend*.c include/ CMakeLists.txt
  build.sh  scripts/ fetch_deps.sh fetch_models.sh package.sh
  webroot/  index.html style.css app.js kernelsu.js assets/
  lib/<abi>/libclarifae.so          (built; customize.sh installs the matching ABI)
  system/lib*/soundfx/              (filled at install with the device's lib)
  README.md INSTALL.md docs/        (repo docs; excluded from the flashable zip)
```

## Security / SELinux

`persist.clarifae.*` use the `default_prop` context; the included `sepolicy.rule` defensively
grants `audioserver` read on those props and load/exec on the soundfx dirs. The effect only
reads properties and audio buffers — it never touches `/data` from the `audioserver` domain.
`clarifae-ctl` runs as root (from the WebUI shell or `su`).
