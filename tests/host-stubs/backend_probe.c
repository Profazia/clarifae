#include "backend.h"
#include "backend_probe.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int channels, mode, highpass, autogain;
    float strength;
} probe_state_t;
probe_metrics_t probe_metrics;
enum probe_fault probe_fault;
unsigned probe_fault_call;

void probe_clear(void) {
    memset(&probe_metrics, 0, sizeof(probe_metrics));
    probe_fault = PROBE_OK;
    probe_fault_call = 0;
}
static void* create_mode(int mode, int channels) {
    ++probe_metrics.create_attempts[mode];
    if (probe_fault == PROBE_CREATE_ERROR) return NULL;
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
    probe_metrics.last_highpass = st->highpass;
    probe_metrics.last_autogain = st->autogain;
    enum probe_fault fault = !probe_fault_call || probe_fault_call == probe_metrics.processes
        ? probe_fault : PROBE_OK;
    int n = frames * st->channels;
    for (int i = 0; i < n; ++i) {
        if (!isfinite(samples[i]) || fabsf(samples[i]) > 32768.0f)
            ++probe_metrics.bad_inputs;
        samples[i] = fault == PROBE_ERROR ? NAN :
            fault == PROBE_HUGE ? (i % 2 ? -FLT_MAX : FLT_MAX) : samples[i] * st->strength;
    }
    if (fault == PROBE_ERROR) return -1;
    /* Poison the last element to catch partial output before validation. */
    if (n && fault == PROBE_NAN)
        samples[n - 1] = NAN;
    if (n && fault == PROBE_INF)
        samples[n - 1] = INFINITY;
    return 0;
}
static void params(void* state, float strength, float atten, float vad, int hp, int agc) {
    (void)atten;
    (void)vad;
    probe_state_t* st = state;
    st->strength = strength;
    st->highpass = hp;
    st->autogain = agc;
}
static void reset(void* state) {
    (void)state;
    ++probe_metrics.resets;
}
static void destroy(void* state) { ++probe_metrics.destroys; free(state); }
static const clarifae_backend_t backends[] = {
    {"probe-rnnoise", create1, process, params, reset, destroy},
    {"probe-dtln", create2, process, params, reset, destroy},
    {"probe-df", create3, process, params, reset, destroy}};
const clarifae_backend_t* clarifae_backend_rnnoise(void) {
    ++probe_metrics.lookups;
    return &backends[0];
}
const clarifae_backend_t* clarifae_backend_for_mode(int mode) {
    ++probe_metrics.lookups;
    return &backends[mode >= 1 && mode <= 3 ? mode - 1 : 0];
}
