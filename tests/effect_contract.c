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

static void no_backend_work(void) {
    CHECK(probe_metrics.lookups == 0 && probe_metrics.resets == 0 &&
          probe_metrics.processes == 0 && probe_metrics.destroys == 0);
    for (int mode = 1; mode <= 3; ++mode)
        CHECK(probe_metrics.creates[mode] == 0 && probe_metrics.create_attempts[mode] == 0);
}
static void disabled_lifecycle(void) {
    for (int output = 0; output < 2; ++output)
        for (int master = 0; master < 2; ++master)
            for (int mode = 1; mode <= 3; ++mode) {
                host_properties_reset();
                probe_clear();
                const char* gate = master ? "persist.clarifae.enabled" :
                    output ? "persist.clarifae.out_enabled" : "persist.clarifae.in_enabled";
                const char* modes[] = {"1", "2", "3"};
                host_property_set(gate, "0");
                host_property_set("persist.clarifae.mode", modes[mode - 1]);
                host_property_set("persist.clarifae.strength", "25");
                host_property_set("persist.clarifae.out_strength", "75");
                effect_handle_t h = create(output);
                no_backend_work();
                CHECK(status_cmd(h, EFFECT_CMD_INIT, NULL, 0) == 0);
                no_backend_work();
                effect_config_t c = config(1, 0);
                CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == 0);
                no_backend_work();
                CHECK((*h)->command(h, EFFECT_CMD_RESET, 0, NULL, NULL, NULL) == 0);
                no_backend_work();
                CHECK(status_cmd(h, EFFECT_CMD_ENABLE, NULL, 0) == 0);
                no_backend_work();
                float in[8], out[8];
                for (int i = 0; i < 8; ++i) in[i] = 0.25f;
                CHECK(process_float(h, in, out, 8) == 0 && !memcmp(in, out, sizeof(in)));
                no_backend_work();
                /* Unrelated controls and mode writes while off cannot initialize DSP. */
                host_property_set("persist.clarifae.highpass", "0");
                host_property_set("persist.clarifae.mode", modes[(mode + 1) % 3]);
                CHECK(process_float(h, in, out, 8) == 0);
                no_backend_work();
                CHECK(status_cmd(h, EFFECT_CMD_DISABLE, NULL, 0) == 0);
                CHECK(process_float(h, in, out, 8) == -ENODATA);
                no_backend_work();
                CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == 0);
                CHECK((*h)->command(h, EFFECT_CMD_RESET, 0, NULL, NULL, NULL) == 0);
                CHECK(status_cmd(h, EFFECT_CMD_INIT, NULL, 0) == 0);
                CHECK(status_cmd(h, EFFECT_CMD_ENABLE, NULL, 0) == 0);
                no_backend_work();
                int wanted = (mode + 1) % 3 + 1;
                host_property_set(gate, "1");
                CHECK(process_float(h, in, out, 8) == 0);
                CHECK(out[0] == (output ? 0.1875f : 0.0625f));
                CHECK(probe_metrics.creates[wanted] == 1 && probe_metrics.last_mode == wanted);
                /* An existing state flushes once on bypass; selecting a mode while
                 * disabled must not consume its activation when reenabled. */
                host_property_set(gate, "0");
                unsigned lookups = probe_metrics.lookups, calls = probe_metrics.processes;
                CHECK(process_float(h, in, out, 8) == 0 && out[0] == in[0]);
                CHECK(probe_metrics.resets == 1 && probe_metrics.lookups == lookups);
                int next = wanted % 3 + 1;
                host_property_set("persist.clarifae.mode", modes[next - 1]);
                CHECK(process_float(h, in, out, 8) == 0);
                CHECK(probe_metrics.resets == 1 && probe_metrics.lookups == lookups &&
                      probe_metrics.processes == calls && probe_metrics.creates[next] == 0);
                host_property_set(gate, "1");
                CHECK(process_float(h, in, out, 8) == 0);
                CHECK(probe_metrics.last_mode == next && probe_metrics.creates[next] == 1);
                CHECK(AELI.release_effect(h) == 0);
            }
    /* Deliberately retain legacy prewarming when only the host is disabled. */
    host_properties_reset();
    probe_clear();
    effect_handle_t h = create(1);
    effect_config_t c = config(1, 0);
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == 0);
    CHECK(probe_metrics.lookups > 0 && probe_metrics.creates[1] == 1);
    CHECK(AELI.release_effect(h) == 0);
}
static void microphone_controls(void) {
    for (int mode = 1; mode <= 3; ++mode) {
        host_properties_reset();
        probe_clear();
        const char* modes[] = {"1", "2", "3"};
        host_property_set("persist.clarifae.mode", modes[mode - 1]);
        host_property_set("persist.clarifae.strength", "25");
        host_property_set("persist.clarifae.out_strength", "75");
        effect_handle_t capture = create(0), playback = create(1);
        setup(capture, 1, 0);
        setup(playback, 1, 0);
        float in[8] = {0.25f}, out[8];
        for (int hp = 0; hp < 2; ++hp)
            for (int agc = 0; agc < 2; ++agc) {
                host_property_set("persist.clarifae.highpass", hp ? "1" : "0");
                host_property_set("persist.clarifae.autogain", agc ? "1" : "0");
                CHECK(process_float(capture, in, out, 8) == 0 && out[0] == 0.0625f);
                CHECK(probe_metrics.last_highpass == hp && probe_metrics.last_autogain == agc);
                CHECK(probe_metrics.last_mode == mode && probe_metrics.last_strength == 0.25f);
                CHECK(process_float(playback, in, out, 8) == 0 && out[0] == 0.1875f);
                CHECK(probe_metrics.last_highpass == 0 && probe_metrics.last_autogain == 0);
                CHECK(probe_metrics.last_mode == mode && probe_metrics.last_strength == 0.75f);
            }
        host_property_set("persist.clarifae.in_enabled", "0");
        CHECK(process_float(playback, in, out, 8) == 0 && out[0] == 0.1875f);
        CHECK(AELI.release_effect(capture) == 0 && AELI.release_effect(playback) == 0);
    }
}
static void creation_failure(void) {
    for (int output = 0; output < 2; ++output) {
        host_properties_reset();
        probe_clear();
        host_property_set("persist.clarifae.mode", "2");
        probe_fault = PROBE_CREATE_ERROR;
        effect_handle_t h = create(output);
        effect_config_t c = config(1, 0);
        CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &c, sizeof(c)) == (output ? 0 : -ENODEV));
        CHECK(probe_metrics.create_attempts[2] == 1 && probe_metrics.create_attempts[1] == 1);
        CHECK(status_cmd(h, EFFECT_CMD_ENABLE, NULL, 0) == 0);
        float in[8] = {0.25f}, out[8];
        CHECK(process_float(h, in, out, 8) == (output ? 0 : -EINVAL));
        CHECK(probe_metrics.processes == 0);
        if (output) {
            CHECK(!memcmp(in, out, sizeof(in)));
            /* A later property refresh retries synchronously, not on a worker. */
            probe_fault = PROBE_OK;
            host_property_set("persist.clarifae.out_strength", "75");
            CHECK(process_float(h, in, out, 8) == 0 && out[0] == 0.1875f);
            CHECK(probe_metrics.last_mode == 2 && probe_metrics.creates[2] == 1);
        }
        CHECK(AELI.release_effect(h) == 0);
    }
}

int main(void) {
    interleavings();
    float_safety();
    integer_safety();
    configuration();
    disabled_lifecycle();
    microphone_controls();
    creation_failure();
    printf("PASS effect contract/fault injection checks=%u\n", checks);
    return 0;
}
