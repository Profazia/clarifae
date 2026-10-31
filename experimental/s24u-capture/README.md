# Experimental primary-HAL capture reference (SM-S928B)

**Developer reference, not a flashable module, universal compatibility fix, or automatic installer.** Normal Clarifae packaging and defaults are unchanged by this directory. The tested target is an ARM64 Samsung SM-S928B / pineapple, Android 17 (SDK 37), legacy primary audio device API 3.2. Other firmware/HALs need their own ABI/loader audit and rollback plan.

## Why this exists

Three independent gates must be distinguished: effect registration, attachment, and actual PCM processing. Correcting the legacy AELT/control ABI made both descriptors register, but the tested Samsung capture PRE_PROC attachment still received null PCM buffers and processed zero frames. The primary HAL adapter forwards the original module and invokes a private effect context only after successful eligible PCM reads. It does not replace routing or stock AEC.

The legacy ABI/RNNoise fixes are a separate, smaller contribution. This directory preserves the complete tested experimental engine, optional DTLN/DeepFilterNet integrations, regression fixtures and hash-pinned build inputs so integration can be reviewed independently. It deliberately does not wire the adapter into the default installer.

## Source map

- `src/capture_bridge.c`: primary-module forwarding, stream guards, callback pins and singleton open leases; bounded PCM backup/fail-open; off-thread mode construction/reset; requested/actual backend diagnostics.
- `include/audio_hal_abi.h`: independently audited LP64 public prefixes, not replacement vendor allocations. Original receiver pointers and unlisted tails are preserved. Android AArch64 TBI/MTE tags are removed only for maps-range comparisons, never for actual vendor receivers.
- `engine/`: tested private AELI engine and genuine RNNoise/DTLN/DeepFilterNet DSP. Kept separate from the shipping native tree pending architectural review.
- `backends/dtln/`: official model/runtime pins, complete TFLite tensor/scaling checks, per-channel streaming and real-model tests.
- `backends/df/`: checked Rust C boundary and locked tract closure over unmodified pinned official DSP/model; no guessed upstream C ABI.
- `tests/`: forwarding, singleton lifecycle, malformed/partial/error reads, mute/bypass, invalid output, blocked diagnostics/cancellation, exact property snapshots, construction-disable interleavings and deterministic stale-mode tests. Android-property/optional-runtime stubs are clearly test-only.
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

Any integrator must pin their exact stock firmware hashes; independently verify device/stream prefixes and CFI/BTI/PAC/shared-loader behavior; preserve the vendor allocation/receivers; establish vendor property visibility; mirror all control writers; and prove a bounded recovery guard before enabling. Use new immutable RAM snapshots for new versions, retain stock SoundFX files, and fail closed on unsupported firmware, stale previous-boot state or unowned mounts. Never overwrite an active library/lowerdir, force-unmount, disable SELinux, write original partitions, or restart audio during a call. Model creation/destruction and DF reset occur on the control worker.

The switch `persist.vendor.audio.clarifae.adapter` defaults off when absent. Off is **DSP bypass, not an inert loader**: the HAL wrapper and stock loader still need validation. Raw/MMAP/hotword/telephony/unsupported streams are bypassed. Default tested targets are `mic,voice_comm`; Samsung Voice Recorder used CAMCORDER and was not included. No transport scripts, firmware blobs, root exploit, boot automation or automatic deployment are provided here.

## Verified result and limits

See [VERIFICATION.md](VERIFICATION.md). The real built-in MIC path and same-stream VOICE_COMMUNICATION backend switching were exercised with genuine model identities and positive processing counters; stock Qualcomm AEC remained attached. UI flags alone were not accepted as proof.

- RNNoise: lowest-latency/default recommendation; nominal 30 ms algorithm/framing delay at 48 kHz for the pinned model.
- DTLN: 16 kHz voice-band model, approximately 32 ms; strength supported. VAD/highpass/AGC/attenuation are not model controls. Linear SRC has no strong anti-alias filter.
- DeepFilterNet3: approximately 40 ms plus startup/warm-up; strength/attenuation supported. **Experimental soft realtime:** tract allocates about 2,154 times per 10 ms mono hop. One measured callback exceeded its 20 ms budget. No hard-realtime, long-run thermal or glitch-free guarantee; a bounded DSP-worker design remains preferable.

Bluetooth, USB, MMAP/offload, cellular uplink, playback, live stereo capture, perceptual speech quality and cold-boot/reactivation remain unverified. A single supported phone is not a universal compatibility claim. Further device/route results should extend the matrix rather than silently broaden defaults.
