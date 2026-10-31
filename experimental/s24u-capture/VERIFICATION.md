# Verification matrix

Upstream baseline: `505c0a9a5f1578ddea48eb30747dc15d2978b733` (v1.1.2). Original live integration was tested on an ARM64 SM-S928B / pineapple, Android 17 / SDK 37, primary device API 3.2. This is a single-device result, not a certification of other models/firmware. Aggregate metrics below omit private raw recordings and full phone dumps.

## Established live results

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

Source-only relocation/build checks are recorded in the PR body. No fresh phone test is implied by a successful host rebuild. Review and broad firmware/route testing are still required before shipping an automatic integration.
