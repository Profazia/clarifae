/* Standalone AELI tests with generated PCM only. Never records or writes PCM. */
#include "audio_effect.h"
#include "clarifae_uuid.h"
#include "test_common.h"
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static int status_cmd(effect_handle_t h, uint32_t cmd, void* data, uint32_t n) {
    int32_t result = 123;
    uint32_t size = sizeof(result);
    CHECK((*h)->command(h, cmd, n, data, &size, &result) == 0);
    CHECK(size == sizeof(result));
    return result;
}
static effect_config_t config(int rate, int channels, int fp, int output) {
    effect_config_t e = {0};
    uint32_t mask = output ? (channels == 1 ? AUDIO_CHANNEL_OUT_MONO : AUDIO_CHANNEL_OUT_STEREO)
                           : (channels == 1 ? AUDIO_CHANNEL_IN_MONO : AUDIO_CHANNEL_IN_STEREO);
    e.inputCfg.channels = e.outputCfg.channels = mask;
    e.inputCfg.samplingRate = e.outputCfg.samplingRate = rate;
    e.inputCfg.format = e.outputCfg.format = fp ? AUDIO_FORMAT_PCM_FLOAT : AUDIO_FORMAT_PCM_16_BIT;
    e.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    e.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
    e.inputCfg.mask = e.outputCfg.mask = EFFECT_CONFIG_ALL;
    return e;
}
static double now(void) {
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void matrix(audio_effect_library_t* lib, const effect_uuid_t* id, int side) {
    effect_descriptor_t d = {0}, instance;
    CHECK(lib->get_descriptor(id, &d) == 0);
    CHECK(d.apiVersion == 0x20000 && !memcmp(&d.uuid, id, sizeof(*id)));
    CHECK((d.flags & 7) == (side ? 4u : 3u));
    CHECK((d.flags & 0xf000) == 0x5000 && d.name[63] == 0);
    effect_handle_t h = NULL;
    CHECK(lib->create_effect(id, 0, 0, &h) == 0 && h && *h);
    CHECK((*h)->get_descriptor(h, &instance) == 0 && !memcmp(&d, &instance, sizeof(d)));
    CHECK(status_cmd(h, EFFECT_CMD_INIT, NULL, 0) == 0);
    uint32_t size = 0;
    CHECK((*h)->command(h, EFFECT_CMD_ENABLE, 0, NULL, &size, NULL) == -EINVAL);
    int rates[] = {8000, 44100, 48000, 96000, 192000};
    for (int fp = 0; fp < 2; ++fp)
        for (int ch = 1; ch <= 2; ++ch) {
            for (size_t ri = 0; ri < sizeof(rates) / sizeof(rates[0]); ++ri) {
                effect_config_t e = config(rates[ri], ch, fp, side), got;
                CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &e, sizeof(e)) == 0);
                size = sizeof(got);
                CHECK((*h)->command(h, EFFECT_CMD_GET_CONFIG, 0, NULL, &size, &got) == 0);
                CHECK(size == sizeof(got) && !memcmp(&got, &e, sizeof(e)));
                const int frames = 10000; /* larger than the bounded 2048-frame scratch */
                size_t n = (size_t)frames * ch, bytes = n * (fp ? sizeof(float) : sizeof(int16_t));
                void* in = calloc(1, bytes);
                void* out = malloc(bytes + 16);
                CHECK(in && out);
                memset((char*)out + bytes, 0x5a, 16);
                for (size_t i = 0; i < n; ++i) {
                    if (fp)
                        ((float*)in)[i] = 0.05f * sinf((float)i * 0.2f);
                    else
                        ((int16_t*)in)[i] = (int16_t)(1000 * sinf((float)i * 0.2f));
                }
                audio_buffer_t ib = {.frameCount = frames, .raw = in};
                audio_buffer_t ob = {.frameCount = frames, .raw = out};
                CHECK(status_cmd(h, EFFECT_CMD_DISABLE, NULL, 0) == 0);
                CHECK((*h)->process(h, &ib, &ob) == -ENODATA && !memcmp(in, out, bytes));
                CHECK(status_cmd(h, EFFECT_CMD_ENABLE, NULL, 0) == 0);
                CHECK((*h)->process(h, &ib, &ob) == 0);
                if (fp)
                    for (size_t i = 0; i < n; ++i)
                        CHECK(isfinite(((float*)out)[i]) && fabsf(((float*)out)[i]) <= 1.0f);
                for (int i = 0; i < 16; ++i)
                    CHECK(((unsigned char*)out)[bytes + i] == 0x5a);
                ob.frameCount = frames - 1;
                CHECK((*h)->process(h, &ib, &ob) == -EINVAL);
                ib.frameCount = ob.frameCount = SIZE_MAX;
                CHECK((*h)->process(h, &ib, &ob) == -EINVAL);
                free(in);
                free(out);
            }
        }
    effect_config_t invalid = config(48000, 1, 0, side);
    invalid.outputCfg.samplingRate = 16000;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &invalid, sizeof(invalid)) == -EINVAL);
    invalid = config(48000, 1, 0, side);
    invalid.inputCfg.format = invalid.outputCfg.format = AUDIO_FORMAT_PCM_32_BIT;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &invalid, sizeof(invalid)) == -EINVAL);
    invalid = config(48000, 1, 0, side);
    invalid.outputCfg.channels = 0x3f;
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &invalid, sizeof(invalid)) == -EINVAL);
    CHECK((*h)->command(h, EFFECT_CMD_GET_PARAM, 0, NULL, NULL, NULL) == -EINVAL);
    CHECK((*h)->process_reverse(h, NULL, NULL) == -ENOSYS);

    /* Disable flushes recurrent/framing/resampling state, not just the gate. */
    effect_config_t e = config(48000, 2, 1, side);
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &e, sizeof(e)) == 0);
    float in[480 * 2], out[480 * 2];
    for (int i = 0; i < 960; ++i)
        in[i] = 0.2f;
    audio_buffer_t ib = {.frameCount = 480, .f32 = in};
    audio_buffer_t ob = {.frameCount = 480, .f32 = out};
    for (int i = 0; i < 5; ++i)
        CHECK((*h)->process(h, &ib, &ob) == 0);
    CHECK(status_cmd(h, EFFECT_CMD_DISABLE, NULL, 0) == 0);
    CHECK(status_cmd(h, EFFECT_CMD_ENABLE, NULL, 0) == 0);
    memset(in, 0, sizeof(in));
    for (int i = 0; i < 5; ++i) {
        CHECK((*h)->process(h, &ib, &ob) == 0);
        for (int j = 0; j < 960; ++j)
            CHECK(out[j] == 0.0f);
    }
    CHECK(lib->release_effect(h) == 0);
}
static void synthetic_noise(audio_effect_library_t* lib, const effect_uuid_t* id) {
    effect_handle_t h = NULL;
    CHECK(lib->create_effect(id, 0, 0, &h) == 0);
    effect_config_t e = config(48000, 1, 0, 0);
    CHECK(status_cmd(h, EFFECT_CMD_SET_CONFIG, &e, sizeof(e)) == 0);
    CHECK(status_cmd(h, EFFECT_CMD_ENABLE, NULL, 0) == 0);
    double ein = 0, eout = 0, maximum = 0, total = 0;
    uint32_t seed = 123456789;
    int16_t in[480], out[480];
    for (int block = 0; block < 300; ++block) {
        for (int j = 0; j < 480; ++j) {
            seed = seed * 1664525u + 1013904223u;
            in[j] = (int16_t)(((int32_t)(seed >> 16) - 32768) / 4);
        }
        audio_buffer_t ib = {.frameCount = 480, .s16 = in};
        audio_buffer_t ob = {.frameCount = 480, .s16 = out};
        double t = now();
        CHECK((*h)->process(h, &ib, &ob) == 0);
        t = (now() - t) * 1000;
        if (t > maximum)
            maximum = t;
        total += t;
        if (block >= 100)
            for (int j = 0; j < 480; ++j) {
                ein += (double)in[j] * in[j];
                eout += (double)out[j] * out[j];
                CHECK(out[j] != 32767 && out[j] != -32768);
            }
    }
    double reduction = 10 * log10(ein / eout);
    CHECK(isfinite(reduction) && reduction > 1.0);
    printf("synthetic_noise_reduction_db=%.3f host_mean_ms=%.3f host_max_ms=%.3f\n", reduction,
           total / 300, maximum);
    CHECK(lib->release_effect(h) == 0);
}
int main(int argc, char** argv) {
    CHECK(argc == 2);
    void* so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so) {
        fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    audio_effect_library_t* lib = dlsym(so, "AELI");
    CHECK(lib && lib->tag == 0x41454c54 && lib->version == 0x30000);
    effect_uuid_t ids[] = {CLARIFAE_IMPL_UUID, CLARIFAE_OUT_IMPL_UUID};
    CHECK(lib->get_descriptor(NULL, NULL) == -EINVAL);
    effect_uuid_t bad = {0};
    effect_handle_t h = NULL;
    CHECK(lib->create_effect(&bad, 0, 0, &h) == -EINVAL && !h);
    for (int side = 0; side < 2; ++side)
        matrix(lib, &ids[side], side);
    synthetic_noise(lib, &ids[0]);
    CHECK(lib->release_effect(NULL) == -EINVAL);
    CHECK(dlclose(so) == 0);
    printf("PASS AELI synthetic smoke checks=%u\n", checks);
    return 0;
}
