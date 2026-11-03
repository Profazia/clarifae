/* Generated PCM + fake backend only: no device, hardware routing or recordings. */
#include "audio_effect.h"
#include "clarifae_uuid.h"
#include "host-stubs/backend_probe.h"
#include "host-stubs/properties.h"
#include "test_common.h"
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

extern audio_effect_library_t AELI;
static unsigned scenarios;
static int command(effect_handle_t h, uint32_t cmd, void* data, uint32_t bytes) {
    int32_t status = 123;
    uint32_t size = sizeof(status);
    CHECK((*h)->command(h, cmd, bytes, data, &size, &status) == 0);
    CHECK(size == sizeof(status));
    return status;
}
static effect_handle_t setup(int fp, int channels, int accumulate, int host, int gate) {
    host_properties_reset();
    probe_clear();
    host_property_set("persist.clarifae.enabled", gate == 1 ? "0" : "1");
    host_property_set("persist.clarifae.out_enabled", gate == 2 ? "0" : "1");
    host_property_set("persist.clarifae.out_strength", "75");
    host_property_set("persist.clarifae.loglevel", "0");
    effect_uuid_t id = CLARIFAE_OUT_IMPL_UUID;
    effect_handle_t h = NULL;
    CHECK(AELI.create_effect(&id, 0, 0, &h) == 0 && h && *h);
    effect_config_t c = {0};
    c.inputCfg.samplingRate = c.outputCfg.samplingRate = 48000;
    c.inputCfg.channels = c.outputCfg.channels = 0x80000000u | ((1u << channels) - 1u);
    c.inputCfg.format = c.outputCfg.format = fp ? AUDIO_FORMAT_PCM_FLOAT : AUDIO_FORMAT_PCM_16_BIT;
    c.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    c.outputCfg.accessMode = accumulate ? EFFECT_BUFFER_ACCESS_ACCUMULATE : EFFECT_BUFFER_ACCESS_WRITE;
    c.inputCfg.mask = c.outputCfg.mask = EFFECT_CONFIG_ALL;
    CHECK(command(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == 0);
    CHECK(command(h, host ? EFFECT_CMD_ENABLE : EFFECT_CMD_DISABLE, NULL, 0) == 0);
    return h;
}
static float input_value(size_t i) {
    const float values[] = {2.0f, -1.25f, FLT_MAX, -FLT_MAX, NAN, INFINITY,
                            -INFINITY, 0.25f, 0.0f, -0.0f, -0.5f, 1.0f};
    return values[i % (sizeof(values) / sizeof(values[0]))];
}
static float destination(size_t i) {
    const float values[] = {FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX, 0.75f, NAN,
                            INFINITY, -INFINITY, 2.0f, -2.0f, 0.5f, -0.5f};
    return values[i % (sizeof(values) / sizeof(values[0]))];
}
static float clean(float v) { return isfinite(v) ? v : 0.0f; }
static float expected_add(float a, float b) {
    double sum = (double)clean(a) + clean(b);
    return (float)fmax(-(double)FLT_MAX, fmin((double)FLT_MAX, sum));
}
static float wet_value(float v, size_t i, enum probe_fault fault) {
    if (fault == PROBE_HUGE) return i % 2 ? -1.0f : 1.0f;
    return fmaxf(-1.0f, fminf(1.0f, clean(v))) * 0.75f;
}
static void no_disabled_backend(int gate) {
    if (!gate) return;
    CHECK(probe_metrics.lookups == 0 && probe_metrics.resets == 0);
    for (int mode = 1; mode <= 3; ++mode)
        CHECK(probe_metrics.create_attempts[mode] == 0 && probe_metrics.creates[mode] == 0);
}
static void floats(int frames, int channels, int accumulate, int alias, int host,
                   int gate, enum probe_fault fault, unsigned fault_call) {
    effect_handle_t h = setup(1, channels, accumulate, host, gate);
    size_t n = (size_t)frames * channels;
    float* a = malloc((n + 4) * sizeof(*a));
    float* b = malloc((n + 4) * sizeof(*b));
    float* copy = malloc(n * sizeof(*copy));
    CHECK(a && b && copy);
    float* in = a + 2;
    float* out = alias ? in : b + 2;
    a[0] = b[0] = a[n + 2] = b[n + 2] = 8123.5f;
    a[1] = b[1] = a[n + 3] = b[n + 3] = -8123.5f;
    for (size_t i = 0; i < n; ++i) { in[i] = input_value(i); b[i + 2] = destination(i); }
    memcpy(copy, in, n * sizeof(*copy));
    probe_fault = fault;
    probe_fault_call = fault_call;
    audio_buffer_t ib = {.frameCount = (size_t)frames, .f32 = in};
    audio_buffer_t ob = {.frameCount = (size_t)frames, .f32 = out};
    feclearexcept(FE_OVERFLOW);
    CHECK((*h)->process(h, &ib, &ob) == (host ? 0 : -ENODATA));
    int overflow = fetestexcept(FE_OVERFLOW);
    if (overflow) fprintf(stderr, "overflow frames=%d ch=%d acc=%d alias=%d host=%d gate=%d fault=%d\n",
                          frames, channels, accumulate, alias, host, gate, fault);
    CHECK(!overflow);
    unsigned good_frames = 0, bad_chunks = 0;
    for (int off = 0, call = 1; off < frames; off += 2048, ++call) {
        int count = frames - off > 2048 ? 2048 : frames - off;
        enum probe_fault active = !fault_call || fault_call == (unsigned)call ? fault : PROBE_OK;
        int failed = active == PROBE_ERROR || active == PROBE_NAN || active == PROBE_INF;
        int wet = host && !gate && !failed;
        if (wet) good_frames += (unsigned)count;
        if (host && !gate && failed) ++bad_chunks;
        for (size_t i = (size_t)off * channels; i < (size_t)(off + count) * channels; ++i) {
            float expect = wet ? wet_value(copy[i], i, active) : clean(copy[i]);
            if (accumulate) expect = expected_add(alias ? copy[i] : destination(i), expect);
            CHECK(isfinite(out[i]) && out[i] == expect);
            if (!wet && !accumulate && isfinite(copy[i]))
                CHECK(!memcmp(&out[i], &copy[i], sizeof(float))); /* includes signed zero */
        }
    }
    CHECK(probe_metrics.bad_inputs == 0);
    CHECK(probe_metrics.processes == (host && !gate ? (unsigned)(frames + 2047) / 2048 : 0));
    if (host && !gate) CHECK(probe_metrics.resets == bad_chunks);
    no_disabled_backend(gate);
    CHECK(a[0] == 8123.5f && a[1] == -8123.5f && a[n + 2] == 8123.5f && a[n + 3] == -8123.5f);
    CHECK(b[0] == 8123.5f && b[1] == -8123.5f && b[n + 2] == 8123.5f && b[n + 3] == -8123.5f);
    if (!alias) CHECK(!memcmp(in, copy, n * sizeof(*copy)));
    /* DUMP remains observational; only successful DSP advances this counter. */
    FILE* file = tmpfile();
    CHECK(file);
    int fd = fileno(file);
    CHECK((*h)->command(h, EFFECT_CMD_DUMP, sizeof(fd), &fd, NULL, NULL) == 0);
    CHECK(!fseek(file, 0, SEEK_SET));
    char text[512];
    size_t bytes = fread(text, 1, sizeof(text) - 1, file);
    text[bytes] = '\0';
    CHECK(!fclose(file));
    const char* field = strstr(text, "processed_frames=");
    CHECK(field && strtoul(field + strlen("processed_frames="), NULL, 10) == good_frames);
    CHECK(AELI.release_effect(h) == 0);
    free(a); free(b); free(copy);
    ++scenarios;
}
static int16_t integer_value(size_t i) {
    const int16_t values[] = {30000, -30000, 32764, -32768, 0, 120, -120};
    return values[i % (sizeof(values) / sizeof(values[0]))];
}
static void integers(int frames, int accumulate, int alias, int host, int gate, enum probe_fault fault) {
    const int channels = 2;
    effect_handle_t h = setup(0, channels, accumulate, host, gate);
    size_t n = (size_t)frames * channels;
    int16_t* in = malloc((n + 2) * sizeof(*in));
    int16_t* separate = malloc((n + 2) * sizeof(*separate));
    CHECK(in && separate);
    int16_t* out = alias ? in : separate;
    for (size_t i = 0; i < n; ++i) { in[i] = integer_value(i); separate[i] = i % 2 ? -10000 : 10000; }
    in[n] = separate[n] = 12345; in[n + 1] = separate[n + 1] = -12345;
    probe_fault = fault;
    audio_buffer_t ib = {.frameCount = (size_t)frames, .s16 = in};
    audio_buffer_t ob = {.frameCount = (size_t)frames, .s16 = out};
    CHECK((*h)->process(h, &ib, &ob) == (host ? 0 : -ENODATA));
    int wet = host && !gate && (fault == PROBE_OK || fault == PROBE_HUGE);
    for (size_t i = 0; i < n; ++i) {
        int32_t expect = integer_value(i);
        if (wet) expect = fault == PROBE_HUGE ? (i % 2 ? -32768 : 32767) : expect * 3 / 4;
        if (accumulate) expect += alias ? integer_value(i) : (i % 2 ? -10000 : 10000);
        if (expect > 32767) expect = 32767;
        if (expect < -32768) expect = -32768;
        CHECK(out[i] == expect);
        if (!alias) CHECK(in[i] == integer_value(i));
    }
    CHECK(in[n] == 12345 && in[n + 1] == -12345);
    CHECK(separate[n] == 12345 && separate[n + 1] == -12345);
    CHECK(probe_metrics.bad_inputs == 0);
    CHECK(probe_metrics.processes == (host && !gate ? (unsigned)(frames + 2047) / 2048 : 0));
    no_disabled_backend(gate);
    CHECK(AELI.release_effect(h) == 0);
    free(in); free(separate);
    ++scenarios;
}
int main(void) {
    const int sizes[] = {1, 17, 192, 480, 2048, 2049, 4096};
    const int channels[] = {1, 2, 8};
    for (int host = 0; host < 2; ++host)
        for (int gate = 0; gate < 3; ++gate)
            for (int accumulate = 0; accumulate < 2; ++accumulate)
                for (int alias = 0; alias < 2; ++alias)
                    for (size_t sz = 0; sz < sizeof(sizes) / sizeof(sizes[0]); ++sz)
                        for (int fault = PROBE_OK; fault <= PROBE_ERROR; ++fault) {
                            for (size_t ch = 0; ch < sizeof(channels) / sizeof(channels[0]); ++ch)
                                floats(sizes[sz], channels[ch], accumulate, alias, host, gate,
                                       (enum probe_fault)fault, 0);
                            integers(sizes[sz], accumulate, alias, host, gate, (enum probe_fault)fault);
                        }
    /* A later-chunk failure must not abandon the rest or count it as successful DSP. */
    for (int accumulate = 0; accumulate < 2; ++accumulate)
        for (int alias = 0; alias < 2; ++alias)
            for (int fault = PROBE_NAN; fault <= PROBE_ERROR; ++fault)
                if (fault != PROBE_HUGE)
                    floats(4096, 2, accumulate, alias, 1, 0, (enum probe_fault)fault, 2);
    /* Sweep every callback length, rotating access/alias/fault combinations. */
    for (int frames = 1; frames <= 4096; ++frames)
        floats(frames, 1, frames & 1, (frames >> 1) & 1, 1, 0,
               (enum probe_fault)(frames % 5), 0);
    printf("PASS playback total-delivery scenarios=%u checks=%u no_hardware=1\n", scenarios, checks);
    return 0;
}
