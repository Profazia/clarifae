# Legacy effect ABI and streaming tests

This change fixes the portable legacy effect and RNNoise streaming path. It
changes no installer, property defaults, model downloader, SELinux policy or
boot behavior, and adds no device-specific HAL wrapper or dynamic backend plugin.

## Three independent gates

1. **Registration:** the platform's effect factory can load the library and
   enumerate the exact implementation UUID. Exporting an AELI symbol alone is
   not proof that the platform accepted its tag, version or descriptor.
2. **Attachment:** an enabled/configured instance is attached to the intended
   current recording or playback session. Registration and XML entries do not
   prove this.
3. **PCM processing:** that same live instance actually receives PCM, successfully
   calls its backend, and advances its processed-frame counter. A capture effect
   can be attached only as a control interface while a vendor HAL handles PCM
   elsewhere. Such a path needs a separate integration decision.

The tests below inject generated PCM directly into AELI or the backend. They do
not exercise microphones, speakers, policy routing, HIDL/AIDL services or a vendor
HAL. They are **not a universal Samsung fix**, an attachment test, an acoustic A/B,
or proof of system-wide denoising. No new recordings are needed or produced.

## ABI and command contract

- Legacy library tag: AELT (0x41454c54), library API **3.0**; effect control API
  **2.0**. These versions describe different interfaces.
- Both descriptors advertise DIRECT input and output at bits 12 and 14. Capture
  remains PRE_PROC; playback remains POST_PROC. UUIDs are unchanged.
- Compile-time checks cover LP64 and ILP32 native struct sizes and key offsets.
  The library exposes the 3.0 prefix, not a fabricated 3.1 extension.
- SET_CONFIG merges only masked fields, validates rates (8–192 kHz), matching
  channel counts (1–8), positional/index representations, PCM16/float, and access
  modes. Provider callbacks and format conversion between different formats are
  unsupported. GET_CONFIG returns the actual accepted direct-buffer configuration.
- Unsupported parameter commands return an error instead of an empty/fabricated
  successful NS parameter response. This is not a complete Android NS parameter
  implementation. DISABLE flushes state and returns -ENODATA while copying valid
  input. Property bypass copies valid input without claiming the host disabled it.
- Capture float input remains strictly finite and normalized to [-1, 1], including
  bypass; invalid input/destinations and backend errors/nonfinite output return an
  error before writing that chunk. Playback instead always fills valid configured
  output buffers: finite dry mixer headroom above unity is preserved, nonfinite
  input samples and accumulation destinations are locally treated as zero, and
  float accumulation saturates only at +/-FLT_MAX, not unity. The backend receives
  a separate normalized copy (clamped to [-1, 1] before int16-scale conversion).
  Successful wet output remains normalized; dry headroom is not a claim about wet
  processing. A playback backend error or any nonfinite result discards the whole
  affected chunk's scratch, flushes the failed state and delivers original dry
  input, also in-place/accumulating and across later chunks. Failed DSP does not
  advance processed_frames. PCM16 accumulation saturates; all integer output is
  clamped before rounding/conversion, including huge finite backend values.
- The wrapper splits large callbacks into at most 2048 frames. Scratch is reserved
  during configuration, not resized in the steady-state processing path. An error
  in a later capture chunk does not undo already processed earlier chunks.

The ABI values/layouts follow AOSP's
[hardware/audio_effect.h](https://android.googlesource.com/platform/hardware/libhardware/+/refs/heads/main/include_all/hardware/audio_effect.h),
[system/audio_effect.h](https://android.googlesource.com/platform/system/media/+/refs/heads/main/audio/include/system/audio_effect.h)
and [audio_effect-base.h](https://android.googlesource.com/platform/system/media/+/refs/heads/main/audio/include/system/audio_effect-base.h).

## Configuration and real-time limits

Each instance owns exact per-property identity/serial/value snapshots. The code
uses public API 26 __system_property_read_callback, not the private property-serial
API or a process-global consumer cursor. The watcher runs **before** loading
configuration, including creation and SET_CONFIG, so a disable arriving during a
load remains visible to the next check. A read is coherent per property, not a
transaction across multiple properties. The shared diagnostic log level is atomic.
Only the existing persist.clarifae.* keys are used; there are no vendor aliases.
As before, the host must serialize command/process/lifetime operations on a given
instance; atomic logging is not a general per-instance concurrency mechanism.

Fresh master-off or side-off lifecycles perform **zero backend lookup, create or
reset** through creation, INIT, SET_CONFIG, RESET, ENABLE/DISABLE and process.
Format configuration and bounded scratch allocation still occur. A previously
active instance still flushes its existing state on bypass; explicit RESET/INIT
may reset an existing backend. This is a narrow initialization gate, not a worker
or a zero-allocation lifecycle. Host-disabled alone deliberately retains legacy
SET_CONFIG prewarming; it is not covered by the zero-initialization guarantee.
Mode changes observed while off are activated after reenable using the last built
mode rather than just the last observed property value. Playback format setup
remains usable for dry output if all backend creation attempts fail; capture keeps
the setup error. Playback retries creation on the next configuration/property
refresh, not repeatedly on every unchanged callback.

The highpass/autogain properties are microphone controls: capture still receives
their values, while playback always passes HP=0/AGC=0 to its backend. Playback
out_strength and side enable remain independent of input strength/enable. No new
output filter properties, UI/routing changes or device integration are provided.

Live legacy mode semantics are retained: selecting a different available backend
can destroy/create its state on the process thread. Optional backend/model setup,
reset, property reads and logging have not been made hard-real-time safe here.
DTLN and DeepFilterNet remain the original weak, optional linked backends; they
are not loaded as plugins. A separate lifecycle/worker design would need its own
synchronization and review, not a silent change to this legacy interface.

RNNoise scheduling advances sample by sample, so callback partitions no longer
change queue timing. Output is zero-primed instead of substituting current dry
samples during underruns. The dry branch is delayed to align with RNNoise v0.2's
two-frame STFT/spectral lookahead before wet/dry mixing. At 48 kHz the wrapper's
one-frame priming plus RNNoise's two frames is **1440 samples / 30 ms**, including
at zero wet strength. Full bypass remains immediate passthrough. Other rates add
resampling/priming rounding; this is not a device end-to-end latency measurement.
Valid delayed/filter tails are preserved, and reset clears recurrent, resampler,
HP/AGC, queue and delayed-dry state.

Timing tests intentionally target the **RNNoise v0.2 release**. Audit the delay
and rerun them before substituting another release/model. The existing dependency
fetch behavior is unchanged; it does not pin every possible upstream checkout.
The v0.2 release tarball used for the gates has SHA-256:

90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37

## Reproduce without copying dependencies into this repository

Use an already prepared RNNoise v0.2 release source tree, including trained source
tables and any headers required by that release. No weights, dependency trees or
compiled libraries are part of this change. All paths below are examples to replace.

~~~sh
export RNNOISE_DIR=/path/to/prepared/rnnoise-0.2
export ANDROID_NDK_HOME=/path/to/android-ndk
ANDROID_API=26 ABIS="arm64-v8a armeabi-v7a x86_64" ./build.sh
./tests/run-host.sh
~~~

The Android build honors RNNOISE_DIR and does not fetch when an explicit tree is
supplied. Artifacts are ignored under build/ and lib/. Host gates require CMake,
a C11 Clang/GCC compiler and a build tool. They never download dependencies or
access a device. Sanitizers default ON; use a separate BUILD_DIR and
CLARIFAE_SANITIZERS=OFF for an unsanitized comparison. On macOS, Apple ASan has no
LeakSanitizer, so leak detection is disabled; AddressSanitizer and UBSan remain on.
The project's test targets build with -Wall -Wextra -Werror; third-party RNNoise
sources do not inherit -Werror.

- effect_smoke dynamically loads a host test library, checks both descriptors,
  masked config/readback and malformed buffers, PCM16/float mono/stereo at
  8/44.1/48/96/192 kHz, large callbacks, bit-exact disabled copies, stale-state
  flushing and deterministic noise processing. Genuine RNNoise playback output
  is bit-identical across all four microphone HP/AGC combinations. Its timing is
  host-only.
- backend_pipeline checks bit-identical fixed/irregular partitions at
  8/16/44.1/48/96/192 kHz (mono/stereo, 0/50% wet), reset-versus-fresh equality,
  aligned dry delay, direct/wrapped impulse delay and nonzero filter tails.
- config_watch checks independent watchers, missing/new properties, serial-sum
  collisions, repeated serials with changed values, defaults/bounds, string
  termination, atomic log-level access and a controlled load/disable interleaving.
- effect_contract uses an explicitly fake backend to test per-instance updates,
  construction/configuration interleavings, immediate 2→3→2 mode activation,
  provider rejection, strict capture finite/range/error behavior, fresh disabled
  lifecycle zero-work assertions for both sides/all three fake modes, delayed mode
  activation after reenable, creation failures, and microphone/playback parameter
  isolation. It does not claim to test real optional runtimes.
- output_matrix uses the fake backend for PCM16/float WRITE/ACCUMULATE, exact
  alias/separate buffers, host enabled/disabled, master/side bypass, 1/17/192/480/
  2048/2049/4096-frame callbacks and 1/2/8-channel float output. It asserts complete
  finite delivery, dry headroom and signed-zero preservation, saturation without
  float overflow, local input/destination repair, normalized backend input, guard
  sentinels and unchanged separate input. Error/NaN/Inf/huge finite fault cases
  include a second-chunk-only fault and successful-DSP counter checks. An extra
  sweep covers every callback length from 1 through 4096, rotating access, alias
  and fake fault combinations.

The host-only property store, logging shim and NULL optional-backend stubs live
under tests/host-stubs and are never included by the Android build. They cannot
prove bionic property behavior or SELinux readability in an actual audio service.
Actual Android loading/execution, optional neural runtimes, device deadlines,
stream attachment and platform PCM routing still require separate validation.
