# Verification matrix

Upstream baseline: `505c0a9a5f1578ddea48eb30747dc15d2978b733` (v1.1.2). Original live integration was tested on an ARM64 SM-S928B / pineapple, Android 17 / SDK 37, primary device API 3.2. This is a single-device result, not a certification of other models/firmware. Aggregate metrics below omit private raw recordings and full phone dumps.

## Original established live results (31 October)

| Test | Observed outcome |
|---|---|
| MIC RNNoise | 479,040 processed frames, 0 DSP errors, 0 deadline overruns; mean/max 2.705/7.102 ms per 20 ms callback |
| MIC DTLN | 574,080 processed frames, 0 DSP errors, 0 deadline overruns; mean/max 3.872/12.601 ms |
| MIC DeepFilterNet3 | 552,960 processed frames plus 22,080 transitional bypass frames; 0 DSP errors, one callback deadline overrun; mean/max 8.325/20.444 ms |
| Same VOICE_COMMUNICATION stream | Real 1→2→3→1 switching, requested = actual backend; 1,151,040 processed frames, 0 DSP errors/deadline overruns; stock Qualcomm AEC retained |
| Normal control OFF→ON→OFF | DSP counters stopped while original audio bytes continued; re-enable processed again, no stale buffered audio |
| Model availability | Ordinary status-json reported all three model flags true after hash-checked publication; actual backend counters independently verified processing |
| Safety | Root retained, SELinux Enforcing, same kernel boot; no reboot, original-partition/policy change or root reinitialization |

RNNoise acoustic A/B: a single approximately 23.9 s recording with controlled high-band noise and OFF→ON→OFF, temporary AGC bypass. Selected 4–7.5 kHz noise-band power fell 4.27 dB vs the before segment and 2.58 dB vs the after segment, with zero clipped samples in selected segments. Baseline drift exists. This is **not** calibrated speech SNR, perceptual quality certification, or evidence for DTLN/DF acoustic quality. `scripts/analyse-capture.py` contains the explicit segmentation/analysis method; no ambient audio is committed.

One DF capture cancelled on Activity pause as designed. Another fixed-delay completion check ran too early; final capture explicitly waited for DONE with DF retained. These failed checks were not relabeled successes. Diagnostic app was stopped after testing, with its pre-existing user microphone grant preserved.

## Synthetic and ABI gates

- 97,797 original Android forwarding/PCM/lifecycle assertions, including artificial tagged addresses; separate host ASan/UBSan coverage with Android API stubs.
- Exact property namespace/value takeover: 24 checks; construction-disable interleavings: 46 checks.
- Barrier-controlled 2→3→2 stale-mode regression activates the latest valid request rather than permanently consuming it. This uses a mock engine, not neural inference.
- Actual Android DTLN models: isolated mono/stereo inference; host production-C DSP suite: 20 real-model tests under UBSan, reference maximum difference 0.001709 PCM16 units; partition/reset/channel independence.
- Actual Android DF3 inference, malformed-model rejection, concurrent contexts, resets, impulse delay and all 16 supported rate/channel combinations were tested; pinned Rust/tract inference allocates and is not hard realtime. DF host ASan was unavailable; do not infer a DF ASan pass from bridge-only sanitizer coverage.

## 3 November source refresh and previously recorded device evidence

This refresh republishes the canonical asynchronous engine/capture bridge, not a new device test. The source identity and previous deployed binary identities are recorded in [SOURCE_SNAPSHOT.md](SOURCE_SNAPSHOT.md). The output software INSERT role, bounded playback float delivery, independent output HP/AGC, engine-owned lifecycle worker, stale-generation rejection, observational stats and once-per-callback adoption supersede the older wrapper-owned mode-worker architecture. Raw audio is delivered while preparing a first backend; a compatible previous model can continue during a mode-only change. Worker zero priming plus 60 ms live dry/15 ms fade do not eliminate the 30–40 ms model delay.

Previously recorded single-device evidence, **not rerun for this public refresh**:

| Scope | Observed outcome and boundary |
|---|---|
| Async cold local communication-mode setup | DF capture construction 1,797→356 ms; playback play() 2,259→66 ms. Frame delivery improved, but these local call-mode captures were zero-valued even with master and adapter bypass. Not proof of working call speech. Normal MIC/MODE_NORMAL separately delivered 162,682 nonzero samples over 3.96 s. |
| Same live local input/output switching | Off→RNNoise→DTLN→DF→off→DF with positive backend counters; capture max positive-read gap 40 ms and max nonblocking app read 2 ms. DF capture callback max 23.569 ms, seven deadline overruns. Playback timeline advancement is not acoustic gap certification. |
| Real 44.1 kHz stereo A2DP software INSERT | RNNoise→DTLN→DF→RNNoise and output OFF→ON without reopening; native requested/actual/pending names plus increasing successful counters. Final sampled successful frames 1,176,588; 0 track underruns; max observed head-progress gap 53 ms. Session output peaks can include an accumulation destination, not isolated denoised-speech SNR. |
| Automatic voice-stream software INSERT (no manually created AudioEffect) | RNNoise, 103,194 processed frames/117 calls; input/output peaks 0.00866174/0.00757894; 0 repaired samples. Track construct 104 ms, play 63 ms, 0 underruns, max observed head-progress gap 252 ms. Not gap-free Bluetooth or acoustic-call certification. |
| Framework resource boundary | A music reservation consumed the full 512 KB effect budget; even a stock equalizer could then fail. A later Visualizer-first test also prevented Clarifae registration: its nonzero samples were raw playback only, not effect proof. The descriptor estimate was not lowered and the framework budget was not raised. Ordinary media was excluded by the separately audited device policy, not by changes to this normal installer. |
| Owner audibility report | On 3 November the owner reported that calls, music and microphone work. **User-reported audibility; app/route unspecified.** This does not resolve every earlier local zero-valued communication test or establish cellular/modem/acoustic certification. |

No new call, recording, device access or settings change was performed for this refresh. The separate UI live-routing/control-manager update is not included or claimed here. Music must remain an explicit integration opt-in; output HP/AGC are independent of microphone settings. No broad route activation is proposed.

Wrapper dump engine fields are last-observed snapshots: after an adapter-off/excluded/muted read they can retain the previous `enabled`/backend values. Current gate/bypass reason and processing-counter deltas, not these cached labels, determine whether PCM was processed. The disabled-state host regression checks adapter-off/bypass reporting. This preserves the canonical bridge source instead of silently presenting a diagnostic-only follow-up as the deployed binary.

## Relocated refresh gates

The reproducible scripts compile this relocated experimental tree, with explicit external RNNoise/NDK paths. `engine-sanitize.sh` exercises fake-backend async/lifetime/property and float total-delivery suites under ASan/UBSan or TSan, plus a real RNNoise-only synthetic smoke under ASan/UBSan. `host-sanitize.sh` exercises the forwarding/capture lifecycle with fake stock PCM, a real RNNoise engine, explicit Android API stubs and separate fault engines. These are not Android, optional-plugin or real-route execution. Exact observed check counts are recorded with the public update comment.

`build.sh` and `build-race-tests.sh` crossbuild ARM64/API26 production/isolated engines, bridge/fake-stock/fault/property helpers and current engine-owned async regressions. The obsolete wrapper-owned mode-worker fixture was replaced, not retained as a falsely representative passing test. Public dependency pins/fetches/licenses are unchanged. No model/runtime/toolchain blobs are committed, and experimental sources/artifacts stay excluded from the normal ZIP.

DF remains **soft realtime**: inference still allocates and exceeds deadlines in some recorded runs (including 72.798 ms in one isolated normal-priority test). ThreadSanitizer/fake-backend passing is not DF ASan, hard-realtime, perceptual-quality, concurrency, USB/MMAP/offload, cellular or cold-boot certification. No reboot/root-persistence guarantee is made. Review and broad firmware/route testing remain prerequisites before automatic integration.
