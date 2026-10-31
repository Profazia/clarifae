/*
 * backend_rnnoise.c — Clarifae's shipping engine (mode 1, "Low latency").
 *
 * Wraps Xiph RNNoise (BSD-3-Clause). RNNoise is a compact RNN denoiser that runs
 * strictly at 48 kHz on 10 ms (480-sample) mono frames, with samples expressed
 * as floats in the int16 range. This backend adapts an arbitrary capture stream
 * to that contract:
 *
 *   deinterleave -> [optional high-pass] -> resample to 48k -> 480-frame buffer
 *     -> rnnoise_process_frame -> wet/dry mix bounded by the attenuation limit
 *     -> [optional auto-gain] -> resample back to session rate -> reinterleave
 *
 * The 480-frame buffering plus resampling introduces a small fixed latency
 * (30 ms at 48 kHz: 10 ms fixed framing + 20 ms RNNoise v0.2 lookahead).
 * Priming emits zeros, never out-of-order current dry audio.
 *
 * Also hosts clarifae_backend_for_mode() (the dispatcher), since rnnoise is the
 * one backend always linked in.
 */
#include "backend.h"
#include "ringbuf.h"
#include "resampler.h"
#include "log.h"

#include "rnnoise.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>

#define RNN_RATE      48000
#define MAX_FRAME     480     /* RNNoise frame size (10 ms @ 48 kHz) */
#define RING_CAP      16384
#define RNN_DELAY_FRAMES 2    /* pinned RNNoise v0.2 STFT + spectral lookahead */
#define SCRATCH_CAP   4096

typedef struct {
    DenoiseState* st;
    resampler_t*  up;     /* session_rate -> 48000 */
    resampler_t*  down;   /* 48000 -> session_rate */
    ringbuf_t     in48;   /* 48k samples awaiting framing */
    ringbuf_t     outq;   /* session-rate processed samples awaiting output */
    float         hp_x1;  /* high-pass history */
    float         hp_y1;
    float         agc_g;  /* auto-gain smoothed gain */
    float         frame_in[MAX_FRAME];
    float         frame_out[MAX_FRAME];
    float         dry_delay[RNN_DELAY_FRAMES][MAX_FRAME];
    int           dry_slot;
} rnn_chan_t;

typedef struct {
    int          channels;
    int          sample_rate;
    int          frame;       /* rnnoise_get_frame_size() */
    rnn_chan_t*  ch;

    /* params */
    float strength;   /* 0..1 wet amount */
    float atten_lin;  /* min retained level when suppressing = 10^(-dB/20) */
    float vad;        /* 0..1 voice-activity gate */
    int   highpass;
    int   autogain;

    /* shared scratch (audio thread only) */
    float scratch48[SCRATCH_CAP];
    float scratchSr[SCRATCH_CAP];
} rnn_state_t;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void chan_free(rnn_chan_t* c) {
    if (c->st)   rnnoise_destroy(c->st);
    if (c->up)   rs_destroy(c->up);
    if (c->down) rs_destroy(c->down);
    rb_free(&c->in48);
    rb_free(&c->outq);
    memset(c, 0, sizeof(*c));
}

static void prime_output(rnn_chan_t* c, int sample_rate) {
    int count = (sample_rate + 99) / 100 + (sample_rate == RNN_RATE ? 0 : 2);
    float zero = 0.0f;
    for (int i = 0; i < count; ++i) rb_write(&c->outq, &zero, 1);
}

static void* rnn_create(int sample_rate, int channels, char* err, int errlen) {
    if (channels < 1 || channels > 8) return NULL;
    if (sample_rate < 8000 || sample_rate > 192000) {
        snprintf(err, errlen, "unsupported sample rate %d", sample_rate);
        return NULL;
    }

    rnn_state_t* s = (rnn_state_t*)calloc(1, sizeof(*s));
    if (!s) { snprintf(err, errlen, "oom state"); return NULL; }

    s->channels    = channels;
    s->sample_rate = sample_rate;
    s->frame       = rnnoise_get_frame_size();
    if (s->frame <= 0 || s->frame > MAX_FRAME) s->frame = MAX_FRAME;

    s->strength = 0.7f;
    s->atten_lin = powf(10.0f, -24.0f / 20.0f);
    s->vad = 0.5f;
    s->highpass = 1;
    s->autogain = 0;

    s->ch = (rnn_chan_t*)calloc((size_t)channels, sizeof(rnn_chan_t));
    if (!s->ch) { free(s); snprintf(err, errlen, "oom chans"); return NULL; }

    for (int i = 0; i < channels; i++) {
        rnn_chan_t* c = &s->ch[i];
        c->st = rnnoise_create(NULL);
        c->up = rs_create(sample_rate, RNN_RATE);
        c->down = rs_create(RNN_RATE, sample_rate);
        c->agc_g = 1.0f;
        if (!c->st || !c->up || !c->down ||
            rb_init(&c->in48, RING_CAP) ||
            rb_init(&c->outq, RING_CAP)) {
            snprintf(err, errlen, "oom channel %d", i);
            for (int j = 0; j <= i; j++) chan_free(&s->ch[j]);
            free(s->ch);
            free(s);
            return NULL;
        }
        prime_output(c, sample_rate);
    }

    CLARIFAE_LOGI("rnnoise backend: %d Hz, %d ch, frame=%d", sample_rate, channels, s->frame);
    return s;
}

static void rnn_set_params(void* st, float strength, float atten_db, float vad,
                           int highpass, int autogain) {
    rnn_state_t* s = (rnn_state_t*)st;
    if (!s) return;
    s->strength  = clampf(strength, 0.0f, 1.0f);
    s->atten_lin = powf(10.0f, -clampf(atten_db, 0.0f, 60.0f) / 20.0f);
    s->vad       = clampf(vad, 0.0f, 1.0f);
    s->highpass  = highpass ? 1 : 0;
    s->autogain  = autogain ? 1 : 0;
}

static void rnn_reset(void* st) {
    rnn_state_t* s = (rnn_state_t*)st;
    if (!s) return;
    for (int i = 0; i < s->channels; i++) {
        rnn_chan_t* c = &s->ch[i];
        rb_reset(&c->in48);
        rb_reset(&c->outq);
        rs_reset(c->up);
        rs_reset(c->down);
        memset(c->dry_delay, 0, sizeof(c->dry_delay));
        c->dry_slot = 0;
        prime_output(c, s->sample_rate);
        c->hp_x1 = c->hp_y1 = 0.0f;
        c->agc_g = 1.0f;
        if (c->st) rnnoise_init(c->st, NULL);
    }
}

static void rnn_destroy(void* st) {
    rnn_state_t* s = (rnn_state_t*)st;
    if (!s) return;
    for (int i = 0; i < s->channels; i++) chan_free(&s->ch[i]);
    free(s->ch);
    free(s);
}

/* Process one 480-sample 48 kHz frame in place (frame_in -> frame_out -> mix). */
static void process_frame(rnn_state_t* s, rnn_chan_t* c) {
    float vadprob = rnnoise_process_frame(c->st, c->frame_out, c->frame_in);

    float w = s->strength;
    float amax = s->atten_lin;       /* retained fraction floor */
    for (int i = 0; i < s->frame; i++) {
        float dry = c->dry_delay[c->dry_slot][i];
        c->dry_delay[c->dry_slot][i] = c->frame_in[i];
        float wet = c->frame_out[i];
        float mixed = dry + (wet - dry) * w;
        /* Retained-level floor, without killing valid delayed/filter tails. */
        float floor = amax * fabsf(dry);
        if (fabsf(mixed) < floor) mixed = copysignf(floor, dry);
        c->frame_out[i] = mixed;
    }
    c->dry_slot = (c->dry_slot + 1) % RNN_DELAY_FRAMES;

    if (s->autogain) {
        float acc = 0.0f;
        for (int i = 0; i < s->frame; i++) acc += c->frame_out[i] * c->frame_out[i];
        float rms = sqrtf(acc / (float)s->frame);
        float desired;
        if (vadprob > s->vad && rms > 1.0f) {
            desired = clampf(4000.0f / rms, 1.0f, 4.0f);   /* boost speech only */
            c->agc_g += (desired - c->agc_g) * 0.05f;       /* slow attack */
        } else {
            c->agc_g += (1.0f - c->agc_g) * 0.02f;          /* relax in silence */
        }
        for (int i = 0; i < s->frame; i++)
            c->frame_out[i] = clampf(c->frame_out[i] * c->agc_g, -32768.0f, 32767.0f);
    }
}

static int rnn_process(void* st, float* inout, int frames) {
    rnn_state_t* s = (rnn_state_t*)st;
    if (!s || !inout || frames < 0) return -1;
    /* Advance one session sample at a time. Framing, resampling and output
     * timing are therefore independent of Android callback boundaries. */
    for (int ci = 0; ci < s->channels; ++ci) {
        rnn_chan_t* c = &s->ch[ci];
        for (int i = 0; i < frames; ++i) {
            float sample = inout[(size_t)i * s->channels + ci];
            if (s->highpass) {
                float y = 0.995f * (c->hp_y1 + sample - c->hp_x1);
                c->hp_x1 = sample;
                c->hp_y1 = y;
                sample = y;
            }
            int upn = 0;
            if (rs_process(c->up, &sample, 1, s->scratch48, SCRATCH_CAP, &upn) ||
                rb_write(&c->in48, s->scratch48, upn) != upn) return -1;
            while (rb_avail(&c->in48) >= s->frame) {
                rb_read(&c->in48, c->frame_in, s->frame);
                process_frame(s, c);
                int downn = 0;
                if (rs_process(c->down, c->frame_out, s->frame, s->scratchSr,
                               SCRATCH_CAP, &downn) ||
                    rb_write(&c->outq, s->scratchSr, downn) != downn) return -1;
            }
            float output;
            if (rb_read(&c->outq, &output, 1) != 1) return -1;
            inout[(size_t)i * s->channels + ci] = clampf(output, -32768.0f, 32767.0f);
        }
    }
    return 0;
}

static const clarifae_backend_t kRnnoise = {
    .name       = "rnnoise",
    .create     = rnn_create,
    .process    = rnn_process,
    .set_params = rnn_set_params,
    .reset      = rnn_reset,
    .destroy    = rnn_destroy,
};

const clarifae_backend_t* clarifae_backend_rnnoise(void) { return &kRnnoise; }

/* Optional runtimes are isolated plugins. A missing/bad optional runtime must
 * not prevent RNNoise or the primary HAL from loading. Called on configure or
 * the adapter control worker, never from process(). */
static pthread_once_t dtln_once=PTHREAD_ONCE_INIT,df_once=PTHREAD_ONCE_INIT;
static const clarifae_backend_t* dtln_plugin;
static const clarifae_backend_t* df_plugin;
static void* dtln_dso;
static void* df_dso;
static const clarifae_backend_t* load_plugin(const char* path,const char* symbol,const char* name,void** dso) {
 *dso=dlopen(path,RTLD_NOW|RTLD_LOCAL);
 if(!*dso){CLARIFAE_LOGW("optional %s runtime load failed: %s",name,dlerror());return NULL;}
 const clarifae_backend_t*(*entry)(void)=NULL;*(void**)(&entry)=dlsym(*dso,symbol);
 const clarifae_backend_t*b=entry?entry():NULL;
 if(!b||!b->name||strcmp(b->name,name)||!b->create||!b->process||!b->set_params||!b->reset||!b->destroy){CLARIFAE_LOGW("optional %s backend ABI invalid",name);return NULL;}
 return b;
}
static void load_dtln(void){dtln_plugin=load_plugin("/vendor/lib64/soundfx/libclarifae_dtln_backend.so","clarifae_backend_dtln","dtln",&dtln_dso);}
static void load_df(void){df_plugin=load_plugin("/vendor/lib64/soundfx/libclarifae_df_backend.so","clarifae_backend_deepfilternet","deepfilternet",&df_dso);}
const clarifae_backend_t* clarifae_backend_for_mode(int mode) {
 if(mode==2){const clarifae_backend_t*b=clarifae_backend_dtln?clarifae_backend_dtln():NULL;if(!b){pthread_once(&dtln_once,load_dtln);b=dtln_plugin;}if(b)return b;CLARIFAE_LOGW("DTLN unavailable; explicit fallback to rnnoise");}
 if(mode==3){const clarifae_backend_t*b=clarifae_backend_deepfilternet?clarifae_backend_deepfilternet():NULL;if(!b){pthread_once(&df_once,load_df);b=df_plugin;}if(b)return b;CLARIFAE_LOGW("DeepFilterNet unavailable; explicit fallback to rnnoise");}
 return clarifae_backend_rnnoise();
}
