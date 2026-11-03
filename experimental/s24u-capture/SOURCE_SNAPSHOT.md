# Source snapshot — 3 November 2026

This is a source-only refresh of the experimental reference. It is not a new
device run, a binary distribution, an installer, or a claim that locally rebuilt
artifacts are byte-identical to a previously deployed binary. Public dependency
fetch manifests, pins and redistribution notices are unchanged.

## Canonical implementation identities

| Source in this directory | SHA-256 |
|---|---|
| `engine/effect.c` | `446c6dd464980d3fd0705bee70313a55533e0eac9d3f0c1a4f06406246017ba0` |
| `src/capture_bridge.c` | `7f33dba0f0a26619ba1317b7a381b9df73e585f6337d372d7e69a0a004624177` |

The implementation source is preserved verbatim. Relocated build glue retains
the explicit external RNNoise path and adds NOW/NODELETE; the local ABI header
declares the AOSP software `EFFECT_FLAG_TYPE_INSERT` value (zero). The output
descriptor's actual flags, not legacy introductory POST_PROC wording in the
source comment, define its current software-session role.

Previously recorded production identities, provided only to distinguish the
device evidence in VERIFICATION.md from a new host/crossbuild:

- Engine: `64ebebbfe8f62ed7a6c7f43cc558e710e916509dad9148191f9eec4f315811e9`
- Capture bridge: `5b889b6abed6b4fa73c28b1bf2c78983d69fc9f3f245e279b0badfbc94c671b6`

No proprietary HAL, PCM, phone dump, model/runtime binary or toolchain is
included. No automatic route overlay or UI routing/control-manager change is included.

## Relocated host checks

All were rebuilt/run from this checkout. macOS Android logging/property APIs are
explicit host stubs; no device or audio hardware was used. Apple ASan does not
provide LeakSanitizer, so no LeakSanitizer result is claimed. Polling checks make
the async count scheduling-dependent; these are observed counts for this run.

| Suite | ASan + UBSan | TSan |
|---|---:|---:|
| Slow/delayed fake-backend async lifecycle | 22,852 checks; 10 contexts freed; max 2 live backend states | 22,817 checks; 10 contexts freed; max 2 live backend states |
| Playback float total delivery | 56 scenarios / 640,625 checks | 56 scenarios / 640,625 checks |
| Disabled master/side lifecycle | 12 scenarios / 553 checks; 0 lookups, 0 creates | 12 scenarios / 553 checks; 0 lookups, 0 creates |
| Real property-reader with fake property store | 24 checks | 24 checks |
| Construction/load disable interleavings | 46 checks; backend lookup aborts if reached | 46 checks; backend lookup aborts if reached |
| Real RNNoise, synthetic PCM | INPUT + OUTPUT prime/fade/off-on/reset/release pass | Not run |
| Capture bridge + fake stock PCM / real RNNoise | 97,881 checks | Not run |
| Capture fail-open fault fixtures | 8 scenarios × 73 checks | Not run |

The async suite uses deliberately slow 300 ms fake constructors and a 40 ms
fake pipeline delay. It tests stale 2→3→2 generations, format changes, close
during construction, nonblocking release, dry-first handoff, failure/fallback,
observational stats and adoption only at process entry. A deterministic barrier
publishes a replacement between the two internal chunks of a 4,096-frame active
callback in BOTH directions; neither chunk adopts it until the next callback.
That deliberately coordinated callback is not included in timing samples. A
negative-control build with deliberately wrong per-chunk adoption fails at the
intended assertion, confirming that the regression detects that mutation.
Fake backends are not
neural-inference proof. Matrix cases cover separate/aliased buffers, WRITE and
ACCUMULATE, 1–4,096-frame callbacks, finite headroom, NaN/Inf, overflow and guard
samples; capture normalization remains strict.

Bridge engine fields in dumps are explicitly last-observed cache entries and
may remain stale after host-disable. The adapter-off/bypass-reason regression
checks current gate reporting; cached backend/enable names alone are not proof
of active processing. See README.md for this diagnostic boundary.

## Android and package gates

- NDK r27b/API26 ARM64 production and isolated engines, production/test bridges,
  fake stock, fault, property, async/matrix/disabled/interleaving helpers built
  and linked. This is cross-compilation, not Android execution.
- Production engine and bridge: AArch64 ELF64, AELI/HMI respectively, only
  expected Android system dependencies, 16 KiB LOAD alignment, NOW/NODELETE.
- Shell syntax and unchanged companion property-patch applicability pass.
- Normal packager ZIP excludes the entire experimental tree, native/build
  sources, caches and toolchains. No shipping library was staged for that
  exclusion-only check; it does not certify an installable module.
- Diff whitespace and staged text/privacy allowlist gates pass. No default
  installer or public fetch/pin/license change.

See VERIFICATION.md for audibility, route, memory-budget and soft-realtime
limitations. No phone test, deployment, merge or universal support is implied.
