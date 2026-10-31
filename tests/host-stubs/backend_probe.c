#include "backend.h"
#include "backend_probe.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int channels, mode;
    float strength;
} probe_state_t;
probe_metrics_t probe_metrics;
enum probe_fault probe_fault;

void probe_clear(void) {
    memset(&probe_metrics, 0, sizeof(probe_metrics));
    probe_fault = PROBE_OK;
}
static void* create_mode(int mode, int channels) {
    probe_state_t* st = calloc(1, sizeof(*st));
    if (st) {
        st->mode = mode;
        st->channels = channels;
        ++probe_metrics.creates[mode];
    }
    return st;
}
#define CREATE(mode)                                                                               \
    static void* create##mode(int rate, int channels, char* err, int len) {                        \
        (void)rate;                                                                                \
        (void)err;                                                                                 \
        (void)len;                                                                                 \
        return create_mode(mode, channels);                                                        \
    }
CREATE(1)
CREATE(2)
CREATE(3)
static int process(void* state, float* samples, int frames) {
    probe_state_t* st = state;
    ++probe_metrics.processes;
    probe_metrics.last_mode = st->mode;
    probe_metrics.last_strength = st->strength;
    if (probe_fault == PROBE_ERROR)
        return -1;
    int n = frames * st->channels;
    for (int i = 0; i < n; ++i)
        samples[i] =
            probe_fault == PROBE_HUGE ? (i % 2 ? -FLT_MAX : FLT_MAX) : samples[i] * st->strength;
    /* Poison the last element to catch partial output before validation. */
    if (n && probe_fault == PROBE_NAN)
        samples[n - 1] = NAN;
    if (n && probe_fault == PROBE_INF)
        samples[n - 1] = INFINITY;
    return 0;
}
static void params(void* state, float strength, float atten, float vad, int hp, int agc) {
    (void)atten;
    (void)vad;
    (void)hp;
    (void)agc;
    ((probe_state_t*)state)->strength = strength;
}
static void reset(void* state) {
    (void)state;
    ++probe_metrics.resets;
}
static void destroy(void* state) { free(state); }
static const clarifae_backend_t backends[] = {
    {"probe-rnnoise", create1, process, params, reset, destroy},
    {"probe-dtln", create2, process, params, reset, destroy},
    {"probe-df", create3, process, params, reset, destroy}};
const clarifae_backend_t* clarifae_backend_rnnoise(void) { return &backends[0]; }
const clarifae_backend_t* clarifae_backend_for_mode(int mode) {
    return &backends[mode >= 1 && mode <= 3 ? mode - 1 : 0];
}
