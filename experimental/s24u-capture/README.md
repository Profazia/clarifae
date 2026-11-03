# Experimental primary-HAL capture reference (SM-S928B)

**Developer reference, not a flashable module, universal compatibility fix, or automatic installer.** Normal Clarifae packaging and defaults are unchanged by this directory. The tested target is an ARM64 Samsung SM-S928B / pineapple, Android 17 (SDK 37), legacy primary audio device API 3.2. Other firmware/HALs need their own ABI/loader audit and rollback plan.

## Why this exists

Three independent gates must be distinguished: effect registration, attachment, and actual PCM processing. Correcting the legacy AELT/control ABI made both descriptors register, but the tested Samsung capture PRE_PROC attachment still received null PCM buffers and processed zero frames. The primary HAL adapter forwards the original module and invokes a private effect context only after successful eligible PCM reads. It does not replace routing or stock AEC.

The legacy ABI/RNNoise fixes are a separate, smaller contribution. This directory preserves the complete tested experimental engine, optional DTLN/DeepFilterNet integrations, regression fixtures and hash-pinned build inputs so integration can be reviewed independently. It deliberately does not wire the adapter into the default installer.

## Source map

- `src/capture_bridge.c`: primary-module forwarding, stream guards, callback pins and singleton open leases; bounded PCM backup/fail-open; adapter/source host-enable gates; requested/actual backend diagnostics.
- `include/audio_hal_abi.h`: independently audited LP64 public prefixes, not replacement vendor allocations. Original receiver pointers and unlisted tails are preserved. Android AArch64 TBI/MTE tags are removed only for maps-range comparisons, never for actual vendor receivers.
- `engine/`: private AELI engine and genuine RNNoise/DTLN/DeepFilterNet DSP. A detached lifecycle worker per context owns model construction, zero priming and destruction for both directions; commands/read/process do not synchronously construct, destroy or reset a neural model. Kept separate from the shipping native tree pending architectural review.
- `backends/dtln/`: official model/runtime pins, complete TFLite tensor/scaling checks, per-channel streaming and real-model tests.
- `backends/df/`: checked Rust C boundary and locked tract closure over unmodified pinned official DSP/model; no guessed upstream C ABI.
- `tests/`: forwarding, singleton lifecycle, malformed/partial/error reads, mute/bypass, invalid output, blocked diagnostics/cancellation, exact property snapshots, construction-disable interleavings and barrier-controlled stale-mode tests. Slow fake backends exercise observational stats, whole-callback adoption, live dry hold/fade, disabled lifecycle and playback total delivery. Android-property/optional-runtime stubs are clearly test-only.
- `control/vendor-property-mirror.patch`: proposed companion change, **not applied automatically**. This is a zero-context review patch (validate with `git apply --check --unidiff-zero`); it is not part of normal installation. A vendor-reading engine must never be installed without updating every normal WebUI/CLI writer and sync path. Otherwise an old vendor alias can defeat the master-disable control.

## Build without touching a phone

Requires CMake, Ninja, a verified RNNoise 0.2 source/model tree, and Android NDK r27b (`27.1.12297006`). No device access, root or credentials are needed for these builds.

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r27b
export RNNOISE_DIR=/path/to/rnnoise-0.2
# Official RNNoise 0.2 release SHA256:
# 90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
bash experimental/s24u-capture/scripts/build.sh
bash experimental/s24u-capture/scripts/build-race-tests.sh
# macOS-only host fixture ASan/UBSan; Android APIs are stubbed:
bash experimental/s24u-capture/scripts/host-sanitize.sh
bash experimental/s24u-capture/scripts/engine-sanitize.sh
SANITIZER=thread BUILD_DIR=/path/to/separate-tsan-build \
  bash experimental/s24u-capture/scripts/engine-sanitize.sh
```

Optional engines are isolated plugins. Fetch public inputs with their checked manifests, build the DF runtime, then build the plugins:

```sh
python3 experimental/s24u-capture/backends/dtln/scripts/fetch_assets.py
experimental/s24u-capture/backends/df/scripts/fetch.sh
# This explicitly installs a pinned isolated Rust toolchain under this backend only:
experimental/s24u-capture/backends/df/scripts/setup-toolchain.sh
experimental/s24u-capture/backends/df/scripts/build-android.sh
bash experimental/s24u-capture/scripts/build-plugins.sh
```

The isolated DF Rust-toolchain bootstrap currently targets macOS ARM64; Linux ports require a separately pinned host toolchain manifest. Do not silently substitute unverified binaries. See each backend DESIGN.md for model contracts, source/licence provenance and synthetic test commands. Downloaded models, libraries, headers, upstream source, caches and build products are ignored, not committed. Preserve third-party notices when packaging fetched assets.

## Runtime integration contract (not an installation recipe)

The adapter expects the independently preserved original primary module at `/vendor/lib64/soundfx/libclarifae_primary_stock.so` and the private engine at `/vendor/lib64/soundfx/libclarifae.so`. The wrapper has a distinct SONAME; stock HMI lookup is handle-specific. Optional plugin dependencies use a tested vendor-visible RUNPATH, exact SONAMEs and immutable model paths. The app installation badges separately probe `/data/adb/clarifae/models/...`; those files do not prove HAL readability or model activation. Both data-path publication and HAL-visible models need checked copies.

Any integrator must pin their exact stock firmware hashes; independently verify device/stream prefixes and CFI/BTI/PAC/shared-loader behavior; preserve the vendor allocation/receivers; establish vendor property visibility; mirror all control writers; and prove a bounded recovery guard before enabling. Use new immutable RAM snapshots for new versions, retain stock SoundFX files, and fail closed on unsupported firmware, stale previous-boot state or unowned mounts. Never overwrite an active library/lowerdir, force-unmount, disable SELinux, write original partitions, or restart audio during a call. The engine worker replaces reset state rather than synchronously resetting a model in a callback. Android builds retain the DSO with NODELETE while detached workers exit.

### Playback and lifecycle scope (3 November update)

The experimental output descriptor is a **software session INSERT**, not a HAL/output-stage POST_PROC. This is intentionally not transplanted into the portable fix. Successful attachment/PCM on one software-session path does not establish automatic compatibility with other policy routes, firmware, offload or MMAP. No routing XML overlay, deployment script or separate UI routing/control-manager change is included here.

Ordinary music/media **must be opt-in**, not automatically denoised. A product integration should keep alarm/ring/notification/system media raw and make any music policy explicit. Output strength is independent; microphone HP/AGC are never inherited by output. This snapshot leaves the normal installer and its existing defaults untouched; it does not automatically install that policy or activate the experimental engine.

Playback dry bypass preserves finite float mixer headroom (including peaks above one), repairs nonfinite values locally, and saturates accumulation overflow. Backend-domain input remains bounded. Capture float input retains strict finite normalized validation. Generation checks reject stale format/mode/disable builds; raw PCM starts immediately while no backend is ready, and a compatible previous backend continues during replacement. Stats do not adopt/relabel models. Adoption occurs once per whole process callback, followed by 60 ms current live dry and a 15 ms fade. This does not remove model delay or guarantee artifact-free transitions.

The descriptor still declares **512 KB**; the tested framework had a 512 KB effects budget, so one reservation could prevent even another stock effect. Do not hide the limit by lowering estimates or silently raise framework limits. Redundant control-only capture attachments and ordinary-media reservations should be avoided by an audited integration. Concurrent third-party effects/simultaneous voice sessions remain unverified. Worker queues are bounded per context, not a proof of a global resource or hard-realtime bound.

The switch `persist.vendor.audio.clarifae.adapter` defaults off when absent. Off is **DSP bypass, not an inert loader**: the HAL wrapper and stock loader still need validation. Raw/MMAP/hotword/telephony/unsupported streams are bypassed. Default tested targets are `mic,voice_comm`; Samsung Voice Recorder used CAMCORDER and was not included. No transport scripts, firmware blobs, root exploit, boot automation or automatic deployment are provided here.

Bridge dump fields `mode`, `backend`, `requested_mode`, `pending_mode`, `configured` and `enabled` are **last-observed engine snapshots**, not current-state queries. An adapter-off/excluded/muted read can disable the host without refreshing that cache, so a dump can still name the previous backend or enabled state. Check the current adapter gate and `bypass_reason`, then same-session processing-counter deltas; never infer active DSP from a cached name/flag alone. Direct engine GET_STATS is observational and separate from this wrapper cache.

## Verified result and limits

See [VERIFICATION.md](VERIFICATION.md). The real built-in MIC path and same-stream VOICE_COMMUNICATION backend switching were exercised with genuine model identities and positive processing counters; stock Qualcomm AEC remained attached. UI flags alone were not accepted as proof.

- RNNoise: lowest-latency/default recommendation; nominal 30 ms algorithm/framing delay at 48 kHz for the pinned model.
- DTLN: 16 kHz voice-band model, approximately 32 ms; strength supported. VAD/highpass/AGC/attenuation are not model controls. Linear SRC has no strong anti-alias filter.
- DeepFilterNet3: approximately 40 ms plus startup/warm-up; strength/attenuation supported. **Experimental soft realtime:** tract allocates about 2,154 times per 10 ms mono hop. One measured callback exceeded its 20 ms budget. No hard-realtime, long-run thermal or glitch-free guarantee; a bounded DSP-worker design remains preferable.

An isolated 44.1 kHz stereo Bluetooth A2DP software-INSERT session and one automatically attached voice-stream playback session now have native processing evidence; see the dated matrix. This is not broad Bluetooth/route or acoustic-call certification. The owner later reported that calls, music and microphone work, without specifying the app/route. Treat that as **user-reported audibility**, not a measured cellular/modem/acoustic result. USB, MMAP/offload, cellular uplink/downlink, concurrent effects, live stereo capture, perceptual speech quality and cold-boot/reactivation remain uncertified. Further results should extend the matrix rather than silently broaden defaults.
