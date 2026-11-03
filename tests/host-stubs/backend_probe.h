/* Fault-injecting test backend: NOT an implementation of any denoiser. */
#ifndef CLF_TEST_BACKEND_PROBE_H
#define CLF_TEST_BACKEND_PROBE_H
enum probe_fault { PROBE_OK, PROBE_NAN, PROBE_INF, PROBE_HUGE, PROBE_ERROR, PROBE_CREATE_ERROR };
typedef struct {
    unsigned processes, resets, lookups, destroys, bad_inputs;
    unsigned creates[4], create_attempts[4];
    int last_mode, last_highpass, last_autogain;
    float last_strength;
} probe_metrics_t;
extern probe_metrics_t probe_metrics;
extern enum probe_fault probe_fault;
/* Zero faults every call; otherwise fault only this 1-based process call. */
extern unsigned probe_fault_call;
void probe_clear(void);
#endif
