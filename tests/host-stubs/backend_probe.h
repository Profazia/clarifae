/* Fault-injecting test backend: NOT an implementation of any denoiser. */
#ifndef CLF_TEST_BACKEND_PROBE_H
#define CLF_TEST_BACKEND_PROBE_H
enum probe_fault { PROBE_OK, PROBE_NAN, PROBE_INF, PROBE_HUGE, PROBE_ERROR };
typedef struct {
    unsigned processes;
    unsigned resets;
    unsigned creates[4];
    int last_mode;
    float last_strength;
} probe_metrics_t;
extern probe_metrics_t probe_metrics;
extern enum probe_fault probe_fault;
void probe_clear(void);
#endif
