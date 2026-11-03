# DeepFilterNet3 checked runtime

## Inputs and ABI

`pins.env`, `UPSTREAM-SHA256SUMS`, `runtime/Cargo.lock` and `DEPENDENCIES.tsv` pin official source, model, crate closure and licences. Source: Rikorose/DeepFilterNet commit `d375b2d8309e0935d165700c91da9de862a99c31`, MIT OR Apache-2.0. Model: official DeepFilterNet3_onnx.tar.gz, 7,983,136 bytes, SHA256 `c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616`. Exact size/hash are checked **before** parsing. Keep all relevant transitive notices in any binary redistribution.

The old Clarifae integration guessed a two-argument df_create; upstream actually uses three. Upstream process returns local SNR, not an error status; its C API also allows panic/unwrap paths across extern-C and lacks a complete public reset. Therefore this runtime does **not** compile/use upstream capi. `runtime/src/lib.rs` directly wraps the same unmodified official DfTract DSP with checked `clarifae_df_*` symbols defined in `include/clarifae_df.h`. `libdf.so` must be this checked runtime, not an arbitrary upstream libdf binary. Backend identity is DeepFilterNet3/tract-0.21.4/clarifae-checked-v1.

The minimal pinned-libDF manifest changes dependency selection only, excluding training, Python, audio-container and embedded-model features. Cargo uses --locked. No global logger is installed; logging is compile-time disabled. Android dependencies are exactly libc/libdl/libm; no C++ STL, ONNX Runtime, external BLAS, Python or RNNoise.

## DSP/lifetime

The production C adapter is `../../engine/backend_deepfilternet.c`, guarded by CLARIFAE_WITH_DF.

- Independent mono neural/STFT/SRC/FIFO state per channel and stream, not downmix/fanout. Supported rates: 8/16/24/32/44.1/48/88.2/96 kHz; one or two channels.
- Normalize int16-range PCM before inference. Validate pointers/lengths/non-aliasing, finite normalized input, parameters, every process Result/output/SNR; successful-hop counters prove real processing.
- Ordinary Rust panics are caught **inside** the C ABI; inference faults poison the stream and zero the callback. This is not a sandbox: OOM aborts, signals, UB and invalid foreign pointers cannot be caught.
- Reset clones a pristine pre-created graph state, including all GRU/convolution/normalization/STFT history. Upstream DfTract::init alone is not a complete reset. Serialize resets/params/processing and fence lifetime.
- Model delay is 30 ms plus fixed 10 ms FIFO priming at 48 kHz. Strength mixes aligned dry/wet, including strength=0 delayed original content. Other rates have two extra SRC margin samples. Linear SRC has no strong antialias filter; prefer native 48 kHz.
- Strength and attenuation limit are supported. Attenuation<=0 means no cap (100 dB); avoid the upstream undelayed 0-dB shortcut. VAD/highpass/AGC are not implemented controls.

**Not hard realtime.** Pinned tract inference performs approximately 2,154 allocation/reallocation calls per 10 ms mono hop. Creation, destruction and reset also allocate and belong on a serialized control worker. The reference moves those lifecycle operations off the read callback, but ordinary inference still allocates there. A bounded DSP worker is preferable for strict isolation. Original live DF test had one 20.444 ms callback versus its 20 ms budget; long-run thermal/glitch-free performance is unverified.

## Build/test

From the repository root, after setting ANDROID_NDK_HOME to NDK r27b:

```sh
experimental/s24u-capture/backends/df/scripts/fetch.sh
# Explicit isolated Rust 1.85.1/rustup bootstrap, macOS ARM64 only:
experimental/s24u-capture/backends/df/scripts/setup-toolchain.sh
experimental/s24u-capture/backends/df/scripts/build-host.sh
experimental/s24u-capture/backends/df/scripts/test-host.sh
experimental/s24u-capture/backends/df/scripts/test-host-ubsan.sh
experimental/s24u-capture/backends/df/scripts/build-android.sh
```

Toolchains, Cargo cache, downloaded official source/model and outputs stay inside this backend and are ignored. The bootstrap never edits global shell configuration. Other host toolchain manifests require independent pinning. Runtime and C backend tests cover malformed models/buffers, actual model inference, independent/concurrent contexts, resets, partition invariance, aligned strength, all 16 rate/channel combinations and impulse delay. Rust dependencies are not instrumented by the C UBSan test. DF host ASan was unavailable in the original environment; no DF ASan/LSan pass is claimed.

`test-android-local.sh` only invokes synthetic binaries already in its working directory; it performs no microphone/service/module actions. Never package test helpers as production libraries. Default vendor model path is `/vendor/lib64/soundfx/clarifae_models/df/DeepFilterNet3_onnx.tar.gz`; verify exact runtime SONAME/namespace, immutable assets and lifetime-protected model-hop counters on each target. Overall live results/route limits are in ../../VERIFICATION.md.
