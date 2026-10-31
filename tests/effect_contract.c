/* Command/property interleavings and malformed PCM against a fake backend.
 * Actual RNNoise is exercised separately by effect_smoke/backend_pipeline. */
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

static int status_cmd(effect_handle_t h, uint32_t cmd, void* data, uint32_t bytes) {
    int32_t status = 123;
    uint32_t size = sizeof(status);
    CHECK((*h)->command(h, cmd, bytes, data, &size, &status) == 0);
    CHECK(size == sizeof(status));
    return status;
}
static effect_config_t config(int fp, int accumulate) {
    effect_config_t c = {0};
    c.inputCfg.samplingRate = c.outputCfg.samplingRate = 48000;
    c.inputCfg.channels = c.outputCfg.channels = AUDIO_CHANNEL_IN_MONO;
    c.inputCfg.format = c.outputCfg.format = fp ? AUDIO_FORMAT_PCM_FLOAT : AUDIO_FORMAT_PCM_16_BIT;
    c.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    c.outputCfg.accessMode =
        accumulate ? EFFECT_BUFFER_ACCESS_ACCUMULATE : EFFECT_BUFFER_ACCESS_WRITE;
    c.inputCfg.mask = c.outputCfg.mask = EFFECT_CONFIG_ALL;
    return c;
}
static effect_handle_t create(int output) {
    effect_uuid_t id = CLARIFAE_IMPL_UUID, out_id = CLARIFAE_OUT_IMPL_UUID;
    effect_handle_t h = NULL;
    CHECK(AELI.create_effect(output ? &out_id : &id, 0, 0, &h) == 0);
    CHECK(h && *h);
    return h;
}
static void setup(effect_handle_t h, int fp, int accumulate) {
    effect_config_t c = config(fp, accumulate);
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == 0);
    CHECK(status_cmd(h, EFFECT_CMD_ENABLE, NULL, 0) == 0);
}
static int process_float(effect_handle_t h, float* in, float* out, size_t n) {
    audio_buffer_t ib = {.frameCount = n, .f32 = in};
    audio_buffer_t ob = {.frameCount = n, .f32 = out};
    return (*h)->process(h, &ib, &ob);
}
static uint64_t processed_frames(effect_handle_t h) {
    FILE* file = tmpfile();
    CHECK(file);
    int fd = fileno(file);
    CHECK((*h)->command(h, EFFECT_CMD_DUMP, sizeof(fd), &fd, NULL, NULL) == 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    char text[512];
    size_t bytes = fread(text, 1, sizeof(text) - 1, file);
    text[bytes] = '\0';
    CHECK(fclose(file) == 0);
    const char* field = strstr(text, "processed_frames=");
    CHECK(field);
    return strtoull(field + strlen("processed_frames="), NULL, 10);
}
static void disable_during_load(void) {
    CHECK(host_property_snapshot_reads() > 0);
    host_property_set("persist.clarifae.enabled", "0");
}
static void interleavings(void) {
    host_properties_reset();
    probe_clear();
    host_property_set("persist.clarifae.enabled", "1");
    host_property_after_get("persist.clarifae.enabled", disable_during_load);
    effect_handle_t a = create(0); /* verifies creation watches before loading */
    setup(a, 1, 0);
    float in[8], out[8];
    for (int i = 0; i < 8; ++i)
        in[i] = 0.25f;
    CHECK(process_float(a, in, out, 8) == 0);
    CHECK(probe_metrics.processes == 0 && !memcmp(in, out, sizeof(in)));

    host_property_set("persist.clarifae.enabled", "1");
    host_property_after_get("persist.clarifae.enabled", disable_during_load);
    setup(a, 1, 0); /* old enabled read, then disable before load completed */
    CHECK(process_float(a, in, out, 8) == 0);
    CHECK(probe_metrics.processes == 0 && !memcmp(in, out, sizeof(in)));

    host_property_set("persist.clarifae.enabled", "1");
    effect_handle_t b = create(0), o = create(1);
    setup(b, 1, 0);
    setup(o, 1, 0);
    host_property_set("persist.clarifae.strength", "25");
    host_property_set("persist.clarifae.out_strength", "75");
    CHECK(process_float(a, in, out, 8) == 0 && out[0] == 0.0625f);
    CHECK(process_float(b, in, out, 8) == 0 && out[0] == 0.0625f);
    CHECK(process_float(o, in, out, 8) == 0 && out[0] == 0.1875f);
    host_property_set("persist.clarifae.in_enabled", "0");
    unsigned processed = probe_metrics.processes, resets = probe_metrics.resets;
    CHECK(process_float(a, in, out, 8) == 0 && out[0] == in[0]);
    CHECK(process_float(b, in, out, 8) == 0 && out[0] == in[0]);
    CHECK(probe_metrics.processes == processed && probe_metrics.resets == resets + 2);
    CHECK(process_float(o, in, out, 8) == 0 && out[0] == 0.1875f);
    host_property_set("persist.clarifae.in_enabled", "1");
    CHECK(process_float(a, in, out, 8) == 0 && out[0] == 0.0625f);
    /* Legacy mode changes still activate on the next process callback. */
    int modes[] = {2, 3, 2};
    const char* values[] = {"2", "3", "2"};
    for (int i = 0; i < 3; ++i) {
        host_property_set("persist.clarifae.mode", values[i]);
        CHECK(process_float(a, in, out, 8) == 0);
        CHECK(probe_metrics.last_mode == modes[i]);
    }
    CHECK(probe_metrics.creates[2] == 2 && probe_metrics.creates[3] == 1);
    CHECK(AELI.release_effect(a) == 0);
    CHECK(AELI.release_effect(b) == 0);
    CHECK(AELI.release_effect(o) == 0);
}

static void float_safety(void) {
    host_properties_reset();
    probe_clear();
    effect_handle_t h = create(0);
    setup(h, 1, 0);
    float in[8] = {0}, out[8];
    float invalid[] = {NAN, INFINITY, -INFINITY, FLT_MAX, -FLT_MAX, 1.01f, -1.01f};
    for (int bypass = 0; bypass < 2; ++bypass) {
        CHECK(status_cmd(h, bypass ? EFFECT_CMD_DISABLE : EFFECT_CMD_ENABLE, NULL, 0) == 0);
        for (size_t v = 0; v < sizeof(invalid) / sizeof(invalid[0]); ++v) {
            for (int i = 0; i < 8; ++i)
                out[i] = 0.125f;
            in[7] = invalid[v];
            unsigned before = probe_metrics.processes;
            CHECK(process_float(h, in, out, 8) == -EINVAL);
            CHECK(probe_metrics.processes == before);
            for (int i = 0; i < 8; ++i)
                CHECK(out[i] == 0.125f);
        }
    }
    CHECK(status_cmd(h, EFFECT_CMD_ENABLE, NULL, 0) == 0);
    in[7] = 1.0f;
    in[0] = -1.0f;
    CHECK(process_float(h, in, out, 8) == 0);
    CHECK(processed_frames(h) == 8);
    for (int fault = PROBE_NAN; fault <= PROBE_INF; ++fault) {
        probe_fault = (enum probe_fault)fault;
        for (int i = 0; i < 8; ++i)
            out[i] = 0.125f;
        CHECK(process_float(h, in, out, 8) == -EINVAL);
        for (int i = 0; i < 8; ++i)
            CHECK(out[i] == 0.125f);
    }
    probe_fault = PROBE_ERROR;
    CHECK(process_float(h, in, out, 8) == -EINVAL);
    CHECK(processed_frames(h) == 8);
    probe_fault = PROBE_HUGE;
    CHECK(process_float(h, in, out, 8) == 0);
    CHECK(processed_frames(h) == 16);
    for (int i = 0; i < 8; ++i)
        CHECK(out[i] == (i % 2 ? -1.0f : 1.0f));
    setup(h, 1, 1);
    for (int i = 0; i < 8; ++i)
        out[i] = i % 2 ? -0.5f : 0.5f;
    CHECK(process_float(h, in, out, 8) == 0);
    for (int i = 0; i < 8; ++i)
        CHECK(out[i] == (i % 2 ? -1.5f : 1.5f));
    out[7] = NAN;
    unsigned before = probe_metrics.processes;
    CHECK(process_float(h, in, out, 8) == -EINVAL);
    CHECK(probe_metrics.processes == before);
    CHECK(AELI.release_effect(h) == 0);
}

static void integer_safety(void) {
    host_properties_reset();
    probe_clear();
    effect_handle_t h = create(0);
    int16_t in[8] = {0}, out[8];
    audio_buffer_t ib = {.frameCount = 8, .s16 = in};
    audio_buffer_t ob = {.frameCount = 8, .s16 = out};
    for (int accumulate = 0; accumulate < 2; ++accumulate) {
        setup(h, 0, accumulate);
        for (int i = 0; i < 8; ++i)
            out[i] = i % 2 ? -10 : 10;
        probe_fault = PROBE_HUGE;
        feclearexcept(FE_INVALID);
        CHECK((*h)->process(h, &ib, &ob) == 0);
        CHECK(!fetestexcept(FE_INVALID)); /* no out-of-range lrintf first */
        for (int i = 0; i < 8; ++i)
            CHECK(out[i] == (i % 2 ? -32768 : 32767));
        probe_fault = PROBE_NAN;
        for (int i = 0; i < 8; ++i)
            out[i] = 123;
        CHECK((*h)->process(h, &ib, &ob) == -EINVAL);
        for (int i = 0; i < 8; ++i)
            CHECK(out[i] == 123);
    }
    CHECK(status_cmd(h, EFFECT_CMD_DISABLE, NULL, 0) == 0);
    for (int i = 0; i < 8; ++i) {
        in[i] = i % 2 ? -30000 : 30000;
        out[i] = in[i];
    }
    CHECK((*h)->process(h, &ib, &ob) == -ENODATA);
    for (int i = 0; i < 8; ++i)
        CHECK(out[i] == (i % 2 ? -32768 : 32767));
    CHECK(AELI.release_effect(h) == 0);
}

static int32_t provider(void* cookie, audio_buffer_t* buffer) {
    (void)cookie;
    (void)buffer;
    return 0;
}
static void configuration(void) {
    host_properties_reset();
    probe_clear();
    effect_handle_t h = create(0);
    int16_t in[8] = {0}, out[8] = {0};
    audio_buffer_t ib = {.frameCount = 8, .s16 = in};
    audio_buffer_t ob = {.frameCount = 8, .s16 = out};
    CHECK((*h)->process(h, &ib, &ob) == -EINVAL);
    effect_config_t c = config(0, 0);
    c.inputCfg.buffer = ib;
    c.outputCfg.buffer = ob;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == 0);
    CHECK(status_cmd(h, EFFECT_CMD_ENABLE, NULL, 0) == 0);
    CHECK((*h)->process(h, NULL, NULL) == 0); /* configured direct buffers */
    effect_config_t partial = {0}, got;
    partial.inputCfg.mask = partial.outputCfg.mask = EFFECT_CONFIG_ACC_MODE;
    partial.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    partial.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_ACCUMULATE;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &partial, sizeof(partial)) == 0);
    uint32_t size = sizeof(got);
    CHECK((*h)->command(h, EFFECT_CMD_GET_CONFIG, 0, NULL, &size, &got) == 0);
    CHECK(got.inputCfg.buffer.raw == in && got.outputCfg.buffer.raw == out);
    CHECK(got.inputCfg.samplingRate == 48000 && got.inputCfg.channels == AUDIO_CHANNEL_IN_MONO);
    CHECK(got.outputCfg.accessMode == EFFECT_BUFFER_ACCESS_ACCUMULATE);
    c.inputCfg.bufferProvider.getBuffer = provider;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == -ENOSYS);
    c.inputCfg.bufferProvider.getBuffer = NULL;
    c.outputCfg.bufferProvider.releaseBuffer = provider;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == -ENOSYS);
    c.outputCfg.bufferProvider.releaseBuffer = NULL;
    c.inputCfg.channels = c.outputCfg.channels = 0x40000001u;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == -EINVAL);
    c.inputCfg.channels = c.outputCfg.channels = 0x80000003u;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == 0); /* index stereo */
    c.inputCfg.channels = c.outputCfg.channels = 0x800000ffu;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == 0); /* eight channels */
    c.inputCfg.channels = c.outputCfg.channels = 0x800001ffu;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == -EINVAL);
    c.inputCfg.mask = 0x8000;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == -EINVAL);
    CHECK((*h)->command(h, 9999, 0, NULL, NULL, NULL) == -EINVAL);
    CHECK((*h)->command(h, EFFECT_CMD_GET_PARAM, 0, NULL, NULL, NULL) == -EINVAL);
    CHECK((*h)->command(h, EFFECT_CMD_SET_CONFIG, sizeof(c) - 1, &c, NULL, NULL) == -EINVAL);
    size = 1;
    int32_t reply = 123;
    CHECK((*h)->command(h, EFFECT_CMD_DISABLE, 0, NULL, &size, &reply) == -EINVAL);
    CHECK(reply == 123 && size == 1);
    CHECK(AELI.release_effect(h) == 0);
}

int main(void) {
    interleavings();
    float_safety();
    integer_safety();
    configuration();
    printf("PASS effect contract/fault injection checks=%u\n", checks);
    return 0;
}
