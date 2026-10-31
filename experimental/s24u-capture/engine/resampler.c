#include "resampler.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct resampler {
    int    in_rate;
    int    out_rate;
    double step;     /* input samples advanced per output sample */
    double pos;      /* position of next output, relative to in[0] of the
                        current buffer; may be negative (uses carried history) */
    float  prev;     /* input sample immediately before in[0] (index -1) */
    int    started;
};

resampler_t* rs_create(int in_rate, int out_rate) {
    if (in_rate <= 0 || out_rate <= 0) return NULL;
    resampler_t* rs = (resampler_t*)calloc(1, sizeof(*rs));
    if (!rs) return NULL;
    rs->in_rate = in_rate;
    rs->out_rate = out_rate;
    rs->step = (double)in_rate / (double)out_rate;
    rs->pos = 0.0;
    rs->prev = 0.0f;
    rs->started = 0;
    return rs;
}

void rs_destroy(resampler_t* rs) { free(rs); }

void rs_reset(resampler_t* rs) {
    if (!rs) return;
    rs->pos = 0.0;
    rs->prev = 0.0f;
    rs->started = 0;
}

int rs_max_out(const resampler_t* rs, int in_n) {
    if (rs->in_rate == rs->out_rate) return in_n;
    return (int)((double)in_n * (double)rs->out_rate / (double)rs->in_rate) + 2;
}

int rs_process(resampler_t* rs, const float* in, int in_n,
               float* out, int out_cap, int* out_n) {
    if (in_n <= 0) { *out_n = 0; return 0; }

    /* identity */
    if (rs->in_rate == rs->out_rate) {
        if (in_n > out_cap) { *out_n = 0; return -1; }
        memcpy(out, in, (size_t)in_n * sizeof(float));
        *out_n = in_n;
        return 0;
    }

    if (!rs->started) {
        rs->started = 1;
        rs->prev = in[0];
    }

    int produced = 0;
    const double limit = (double)(in_n - 1) + 1e-9;
    while (rs->pos <= limit) {
        int i = (int)floor(rs->pos);
        double frac = rs->pos - (double)i;
        float left  = (i < 0) ? rs->prev : in[i];
        int ri = i + 1;
        if (ri < 0) ri = 0;
        if (ri > in_n - 1) ri = in_n - 1;
        float right = in[ri];
        if (produced >= out_cap) { *out_n = produced; return -1; }
        out[produced++] = left + (right - left) * (float)frac;
        rs->pos += rs->step;
    }

    /* carry state to the next buffer: index in_n-1 becomes the new index -1 */
    rs->prev = in[in_n - 1];
    rs->pos -= (double)in_n;
    *out_n = produced;
    return 0;
}
