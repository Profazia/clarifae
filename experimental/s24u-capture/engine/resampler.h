/*
 * resampler.h — streaming sample-rate converter.
 *
 * RNNoise operates strictly at 48 kHz, but capture sessions are often 8/16/44.1
 * kHz, so the backend resamples session-rate <-> 48 kHz around the engine.
 *
 * Implementation: continuous linear interpolation with one sample of carried
 * history, so consecutive process() calls join seamlessly (no clicks at block
 * boundaries). Linear is a deliberate trade-off — it is cheap, allocation-free
 * per call, and adds negligible latency, which matters more than the last dB of
 * stop-band rejection for a realtime voice path. (A polyphase/cubic kernel could
 * be dropped in behind this same API if higher fidelity is ever required.)
 */
#ifndef CLARIFAE_RESAMPLER_H
#define CLARIFAE_RESAMPLER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct resampler resampler_t;

resampler_t* rs_create(int in_rate, int out_rate);
void         rs_destroy(resampler_t* rs);
void         rs_reset(resampler_t* rs);

/* Worst-case number of output samples produced for in_n input samples. Use to
 * size the output buffer before calling rs_process(). */
int rs_max_out(const resampler_t* rs, int in_n);

/* Resample in[0..in_n) into out (capacity out_cap). Writes the produced count
 * to *out_n. Returns 0 on success, -1 if out_cap was too small. */
int rs_process(resampler_t* rs, const float* in, int in_n,
               float* out, int out_cap, int* out_n);

#ifdef __cplusplus
}
#endif

#endif /* CLARIFAE_RESAMPLER_H */
