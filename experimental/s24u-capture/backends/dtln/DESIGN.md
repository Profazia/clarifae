# DTLN model/runtime integration

## Pinned official inputs

`provenance.json` contains the exact URLs and SHA256 values for all assets. `scripts/fetch_assets.py` verifies existing files and newly downloaded archive members. Models are the original float32 pair from breizhn/DTLN commit `1de1f15a8b5b7e1c44905618ff2ef70ca8277fbc` (MIT), without conversion or retraining. Runtime is the official Google Maven `com.google.ai.edge.litert:litert:1.2.0` ARM64 AAR, using its native TensorFlow Lite C interpreter API and matching headers. Preserve model, TensorFlow and runtime notices when packaging. No third-party runtime, JVM, GPU, NNAPI, Flex or custom-op dependency.

Use the exact `libtensorflowlite_jni.so` SONAME, not a renamed tensorflowlite_c library. Required native TfLite symbols are exported and load segments are 16-KiB aligned. The official runtime reports internal TfLiteVersion `2.20.0-dev0+selfbuilt`; that is its own version string, not evidence this project built TensorFlow. Dependencies are libc/libm/libdl/liblog, with no libc++_shared.

## DSP contract

The production adapter is `../../engine/backend_dtln.c`, guarded by CLARIFAE_WITH_DTLN. Optional-off compilation has no TFLite dependency.

- Official 512-sample analysis, 128-sample hop, explicit LSTM states and overlap/add ordering at 16 kHz. Independent interpreters/state per channel; no downmix/fanout.
- Every float32 tensor shape/rank/size and unique classification is validated; tensor order is not assumed. Data shapes are [1,1,257] / [1,1,512], states [1,2,128,2]. Every copy/invoke result is checked.
- Input and stage-2 IRFFT are **normalized**, not int16-scale. Scale only after stage 2 and overlap/add.
- Sample-clocked SRC/FIFO processing is partition invariant; fixed zero priming, never current dry output on underrun. Nominal total delay is 512 samples / 32 ms at 16 kHz, slightly more SRC margin at other rates. Linear SRC has no strong antialias filter; prefer native 16 kHz.
- Strength mixes time-aligned delayed dry with wet. Strength=0 preserves original per-channel delayed content without clamping; neural state still advances. Attenuation/VAD/highpass/AGC are not graph controls and are explicitly ignored.
- Reset clears all neural/SRC/OLA/FIFO/dry history and fault latch. Inference errors zero output, return -EIO and latch until reset. Creation/destruction must be off realtime; serialize all state operations and lifetime. Source-level no-allocation discipline is not proof against runtime internal locks/page faults.

## Build/test

From this directory:

```sh
python3 scripts/fetch_assets.py
# Set ANDROID_NDK_HOME to NDK r27b first:
scripts/build_android.sh
# Optional, macOS ARM64 CPython 3.11 test environment:
DTLN_UBSAN=1 scripts/test_host.sh
python3 scripts/fetch_assets.py --verify-only
```

Host tests use a **test-only** C-ABI shim calling the official pinned macOS Python LiteRT wheel; never ship that shim/host wheel. They exercise the production C DSP, FFT, scaling, SRC, mixing and failure handling, not a mock neural model. Original run: 20 real-model checks under UBSan; max reference difference 0.001709 PCM16 units, measured impulse lag 512 samples. The isolated Android probe does not record microphones. Real phone integration results and unverified route limits are in ../../VERIFICATION.md.

Vendor-HAL models default to `/vendor/lib64/soundfx/clarifae_models/dtln/model_{1,2}.tflite`; test macros can override them. A data-directory app badge is not HAL readability or active-backend proof. Use vendor-visible immutable assets, checked hashes, exact dependency resolution and live requested/actual model counters. Runtime placement/RUNPATH/SELinux assumptions must be verified on each target without broad policy changes.
