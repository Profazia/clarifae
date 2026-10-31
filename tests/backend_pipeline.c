/* RNNoise v0.2 release timing and streaming tests, generated PCM only. */
#include "backend.h"
#include "log.h"
#include "rnnoise.h"
#include "test_common.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

_Atomic int clarifae_log_level = 0;
static void* create(int rate, int channels, float wet, int extras) {
    const clarifae_backend_t* b = clarifae_backend_rnnoise();
    char err[200] = {0};
    void* st = b->create(rate, channels, err, sizeof(err));
    if (!st)
        fprintf(stderr, "backend create: %s\n", err);
    CHECK(st);
    b->set_params(st, wet, 24.0f, 0.5f, extras, extras);
    return st;
}
static void process(void* st, int channels, int pattern, float* samples, int count) {
    const clarifae_backend_t* b = clarifae_backend_rnnoise();
    int sizes[] = {480, 256, 192, 2048};
    int irregular[] = {1, 17, 513, 31, 1024, 191, 7, 480};
    int off = 0, k = 0;
    while (off < count) {
        int n = pattern < 4 ? sizes[pattern] : irregular[k++ % 8];
        if (n > count - off)
            n = count - off;
        CHECK(b->process(st, samples + (size_t)off * channels, n) == 0);
        off += n;
    }
}
static void run(int rate, int channels, float wet, int pattern, const float* src, float* dst,
                int count) {
    void* st = create(rate, channels, wet, 0);
    memcpy(dst, src, (size_t)count * channels * sizeof(float));
    process(st, channels, pattern, dst, count);
    clarifae_backend_rnnoise()->destroy(st);
}
static void partition_and_reset(void) {
    const clarifae_backend_t* b = clarifae_backend_rnnoise();
    int rates[] = {8000, 16000, 44100, 48000, 96000, 192000};
    for (size_t ri = 0; ri < sizeof(rates) / sizeof(rates[0]); ++ri) {
        for (int ch = 1; ch <= 2; ++ch) {
            int rate = rates[ri], count = rate / 2;
            size_t n = (size_t)count * ch;
            float* src = malloc(n * sizeof(float));
            float* reference = malloc(n * sizeof(float));
            float* out = malloc(n * sizeof(float));
            CHECK(src && reference && out);
            uint32_t seed = 1234;
            for (size_t i = 0; i < n; ++i) {
                seed = 1664525u * seed + 1013904223u;
                src[i] = (float)((int)(seed >> 16) - 32768) * 0.15f;
            }
            for (int strength = 0; strength < 2; ++strength) {
                float wet = strength ? 0.5f : 0.0f;
                run(rate, ch, wet, 0, src, reference, count);
                if (rate == 48000 && strength == 0) {
                    for (size_t i = 0; i < n; ++i)
                        CHECK(reference[i] == (i < 1440u * ch ? 0 : src[i - 1440u * ch]));
                }
                for (int pattern = 1; pattern < 5; ++pattern) {
                    run(rate, ch, wet, pattern, src, out, count);
                    CHECK(!memcmp(reference, out, n * sizeof(float)));
                }
            }
            /* Reset all recurrent, HP/AGC, resampling, dry-delay and queue state. */
            void* st = create(rate, ch, 0.5f, 1);
            memcpy(out, src, n * sizeof(float));
            process(st, ch, 4, out, count);
            b->reset(st);
            memcpy(out, src, n * sizeof(float));
            process(st, ch, 4, out, count);
            void* fresh = create(rate, ch, 0.5f, 1);
            memcpy(reference, src, n * sizeof(float));
            process(fresh, ch, 4, reference, count);
            CHECK(!memcmp(reference, out, n * sizeof(float)));
            b->destroy(st);
            b->destroy(fresh);
            printf("PASS partition/reset rate=%d channels=%d strengths=0,50 "
                   "chunks=480,256,192,2048,irregular\n",
                   rate, ch);
            free(src);
            free(reference);
            free(out);
        }
    }
}
static void impulse(void) {
    const int count = 20 * 480, at = 5 * 480 + 100;
    float* input = calloc((size_t)count, sizeof(float));
    float* direct = calloc((size_t)count, sizeof(float));
    float* output = calloc((size_t)count, sizeof(float));
    CHECK(input && direct && output);
    input[at] = 16000;
    DenoiseState* st = rnnoise_create(NULL);
    CHECK(st && rnnoise_get_frame_size() == 480);
    for (int off = 0; off < count; off += 480)
        rnnoise_process_frame(st, direct + off, input + off);
    rnnoise_destroy(st);
    run(48000, 1, 1.0f, 4, input, output, count);
    float direct_peak = 0, wrapped_peak = 0;
    int direct_at = -1, wrapped_at = -1;
    double tail = 0;
    for (int i = 0; i < count; ++i) {
        CHECK(isfinite(output[i]) && output[i] >= -32768 && output[i] <= 32767);
        if (fabsf(direct[i]) > direct_peak) {
            direct_peak = fabsf(direct[i]);
            direct_at = i;
        }
        if (fabsf(output[i]) > wrapped_peak) {
            wrapped_peak = fabsf(output[i]);
            wrapped_at = i;
        }
        /* Current dry input is zero here; valid delayed filter tails survive. */
        if (i > at + 1440)
            tail += (double)output[i] * output[i];
    }
    CHECK(direct_at - at == 960);
    CHECK(wrapped_at - at == 1440);
    CHECK(tail > 1.0);
    printf("PASS impulse direct_delay=960 wrapped_delay=1440 samples (30ms@48k) nonzero_tail\n");
    free(input);
    free(direct);
    free(output);
}
int main(void) {
    /* Original weak optional-backend dispatcher, explicit NULL host stubs. */
    CHECK(clarifae_backend_for_mode(1) == clarifae_backend_rnnoise());
    CHECK(clarifae_backend_for_mode(2) == clarifae_backend_rnnoise());
    CHECK(clarifae_backend_for_mode(3) == clarifae_backend_rnnoise());
    partition_and_reset();
    impulse();
    printf("PASS backend streaming checks=%u\n", checks);
    return 0;
}
