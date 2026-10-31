/* Genuine DeepFilterNet3 via the checked, pinned Rust/tract runtime.
 * Not compatible with upstream's unchecked df_create/df_process_frame ABI.
 * create/destroy/reset MUST run on the parent's control worker.
 * process has no C allocations, locks or I/O, but upstream tract allocates
 * per 480-sample hop: this mode is NOT hard-realtime safe. See df/README.md.
 */
#ifdef CLARIFAE_WITH_DF
#include "backend.h"
#include "log.h"
#include "../backends/df/include/clarifae_df.h"
#include "../backends/df/include/clarifae_df_backend_diagnostics.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef DF_MODEL_PATH
#define DF_MODEL_PATH "/vendor/lib64/soundfx/clarifae_models/df/DeepFilterNet3_onnx.tar.gz"
#endif
#define DF_RATE 48000
#define DF_HOP 480
#define DF_WET_CAP 4096
#define DF_MAX_CALLBACK 65536

typedef struct {
    int in_rate, out_rate, started;
    int64_t phase; /* target time relative to current input, in out_rate units */
    float prev;
} df_src_t;

typedef struct {
    CdfState *df;
    int used;
    int wet_head, wet_tail, wet_count, prime;
    int dry_frames, dry_pos;
    _Atomic uint64_t model_hops;
    df_src_t up, down;
    float fin[DF_HOP], fout[DF_HOP], wet[DF_WET_CAP];
    float *dry; /* session-rate mono, algorithm + fixed FIFO delay */
} df_channel_t;
typedef struct {
    int rate, channels, failed;
    float strength, atten_db;
    df_channel_t ch[2]; /* no stereo downmix: independent GRU/STFT/FIFO state */
} df_state_t;

static float df_clip(float v) { return fmaxf(-32768.0f, fminf(32767.0f, v)); }
static void df_err(char *err, int n, const char *msg) { if (err && n > 0) snprintf(err,(size_t)n,"%s",msg); }
static int df_rate_supported(int r) {
    return r==8000 || r==16000 || r==24000 || r==32000 || r==44100 ||
           r==48000 || r==88200 || r==96000;
}
/* Causal linear interpolation, integer phase, one sample of carried history.
 * Output depends on the sample stream, never on caller callback partitions.
 * No anti-alias low-pass: rate-converted modes are voice-quality compromises.
 */
static int df_src_push(df_src_t *r, float x, float *out, int cap) {
    if (!r->started) { r->prev=x; r->started=1; }
    int n=0;
    while (r->phase <= 0) {
        if (n==cap) return -EOVERFLOW;
        out[n++]=x + (x-r->prev)*((float)r->phase/(float)r->out_rate);
        r->phase += r->in_rate;
    }
    r->prev=x;
    r->phase -= r->out_rate;
    return n;
}
static void df_prime(df_state_t *s, df_channel_t *c) {
    c->used=c->wet_head=c->wet_tail=c->wet_count=c->dry_pos=0;
    atomic_store_explicit(&c->model_hops,0,memory_order_relaxed);
    memset(c->fin,0,sizeof(c->fin)); memset(c->fout,0,sizeof(c->fout));
    memset(c->wet,0,sizeof(c->wet));
    memset(c->dry,0,(size_t)c->dry_frames*sizeof(float));
    c->up=(df_src_t){.in_rate=s->rate,.out_rate=DF_RATE};
    c->down=(df_src_t){.in_rate=DF_RATE,.out_rate=s->rate};
    /* Fixed zero priming, never current dry samples on underrun. */
    c->wet_count=c->wet_tail=c->prime;
}
static void df_be_destroy(void *st) {
    df_state_t *s=st; if (!s) return;
    for (int i=0;i<s->channels;++i) { clarifae_df_free(s->ch[i].df); free(s->ch[i].dry); }
    free(s);
}
static void *df_be_create(int rate, int channels, char *err, int errlen) {
    if (!df_rate_supported(rate)) { df_err(err,errlen,"DF unsupported sample rate"); return NULL; }
    if (channels<1 || channels>2) { df_err(err,errlen,"DF supports mono/stereo only"); return NULL; }
    df_state_t *s=calloc(1,sizeof(*s));
    if (!s) { df_err(err,errlen,"DF out of memory"); return NULL; }
    s->rate=rate; s->channels=channels; s->strength=1.0f; s->atten_db=100.0f;
    for (int i=0;i<channels;++i) {
        df_channel_t *c=&s->ch[i]; char why[192]={0};
        c->df=clarifae_df_create(DF_MODEL_PATH,s->atten_db,why,sizeof(why));
        if (!c->df) { df_err(err,errlen,why); goto fail; }
        size_t delay=clarifae_df_delay_samples(c->df);
        if (clarifae_df_frame_length(c->df)!=DF_HOP || clarifae_df_sample_rate(c->df)!=DF_RATE || delay!=1440) {
            df_err(err,errlen,"DF incompatible runtime geometry"); goto fail;
        }
        c->prime=(DF_HOP*rate+DF_RATE-1)/DF_RATE + (rate==DF_RATE ? 0 : 2);
        /* All accepted rates represent the exact 30ms model delay in integers. */
        c->dry_frames=c->prime+(int)(delay*(size_t)rate/DF_RATE);
        c->dry=calloc((size_t)c->dry_frames,sizeof(float));
        if (!c->dry || c->prime>=DF_WET_CAP/2) { df_err(err,errlen,"DF buffer allocation failed"); goto fail; }
        atomic_init(&c->model_hops,0);
        df_prime(s,c);
    }
    df_err(err,errlen,"");
    CLARIFAE_LOGI("deepfilternet ready: %s rate=%d independent_ch=%d hop=%d model_delay48=1440 fifo=%d dry_delay=%d",
                  clarifae_df_backend_name(),rate,channels,DF_HOP,s->ch[0].prime,s->ch[0].dry_frames);
    return s;
fail: df_be_destroy(s); return NULL;
}
static int df_wet_push(df_channel_t *s, float x) {
    if (s->wet_count==DF_WET_CAP) return -EOVERFLOW;
    s->wet[s->wet_tail]=x;
    s->wet_tail=(s->wet_tail+1)%DF_WET_CAP; ++s->wet_count;
    return 0;
}
static int df_hop(df_channel_t *s) {
    float lsnr;
    int rc=clarifae_df_process(s->df,s->fin,DF_HOP,s->fout,DF_HOP,&lsnr);
    if (rc) return rc;
    atomic_fetch_add_explicit(&s->model_hops,1,memory_order_relaxed);
    for (int j=0;j<DF_HOP;++j) {
        float converted[3];
        int n=df_src_push(&s->down,s->fout[j],converted,3);
        if (n<0) return n;
        for (int k=0;k<n;++k) if ((rc=df_wet_push(s,converted[k]))) return rc;
    }
    s->used=0;
    return 0;
}
static int df_be_process(void *st, float *inout, int frames) {
    df_state_t *s=st;
    if (!s || frames<0 || frames>DF_MAX_CALLBACK || (!inout && frames)) return -EINVAL;
    if (!frames) return 0;
    size_t count=(size_t)frames*(size_t)s->channels;
    if (s->failed) { memset(inout,0,count*sizeof(float)); return -EIO; }
    /* Reject before touching model/FIFOs; do not feed NaN/Inf into Rust/FFT. */
    for (size_t i=0;i<count;++i) if (!isfinite(inout[i])) {
        memset(inout,0,count*sizeof(float)); return -EINVAL;
    }
    int rc=0;
    for (int i=0;i<frames;++i) for (int ci=0;ci<s->channels;++ci) {
        df_channel_t *c=&s->ch[ci];
        size_t index=(size_t)i*(size_t)s->channels+(size_t)ci;
        float original=df_clip(inout[index]), raised[8];
        int n=df_src_push(&c->up,original/32768.0f,raised,8);
        if (n<0) { rc=n; goto failure; }
        for (int k=0;k<n;++k) {
            c->fin[c->used++]=raised[k];
            if (c->used==DF_HOP && (rc=df_hop(c))) goto failure;
        }
        if (!c->wet_count) { rc=-EPIPE; goto failure; }
        float wet=c->wet[c->wet_head]*32768.0f;
        c->wet_head=(c->wet_head+1)%DF_WET_CAP; --c->wet_count;
        if (!isfinite(wet)) { rc=-EIO; goto failure; }
        float dry=c->dry[c->dry_pos]; c->dry[c->dry_pos]=original;
        float mixed=(1.0f-s->strength)*dry+s->strength*wet;
        inout[index]=df_clip(mixed);
        if (++c->dry_pos==c->dry_frames) c->dry_pos=0;
    }
    return 0;
failure:
    /* No misaligned dry fallback, partial output or stale stream after failure. */
    s->failed=1; memset(inout,0,count*sizeof(float)); return rc ? rc : -EIO;
}
static void df_be_set_params(void *st, float strength, float atten_db, float vad,
                             int highpass, int autogain) {
    (void)vad; (void)highpass; (void)autogain;
    df_state_t *s=st; if (!s) return;
    if (isfinite(strength)) s->strength=fmaxf(0.0f,fminf(1.0f,strength));
    /* Clarifae 0 means no attenuation CAP; never use upstream's 0dB shortcut
     * (it bypasses with zero delay and would break wet/dry alignment). */
    if (isfinite(atten_db)) {
        float lim=atten_db<=0 ? 100.0f : fmaxf(0.01f,fminf(100.0f,atten_db));
        if (lim!=s->atten_db) {
            for (int ci=0;ci<s->channels;++ci)
                if (clarifae_df_set_atten_lim(s->ch[ci].df,lim)) s->failed=1;
            s->atten_db=lim;
        }
    }
}
static void df_be_reset(void *st) {
    df_state_t *s=st; if (!s) return;
    /* Restores recurrent/norm/STFT state too; allocates in Rust. Worker only. */
    s->failed=0;
    for (int ci=0;ci<s->channels;++ci) {
        if (clarifae_df_reset(s->ch[ci].df)) s->failed=1;
        df_prime(s,&s->ch[ci]);
    }
}
/* Thread-safe diagnostics only; lifecycle remains parent-owned. */
uint64_t clarifae_deepfilternet_model_frames(const void *st, int channel) {
    const df_state_t *s=st;
    if (!s || channel<0 || channel>=s->channels) return 0;
    return atomic_load_explicit(&s->ch[channel].model_hops,memory_order_relaxed);
}
static const clarifae_backend_t kDf={
    .name="deepfilternet",.create=df_be_create,.process=df_be_process,
    .set_params=df_be_set_params,.reset=df_be_reset,.destroy=df_be_destroy
};
const clarifae_backend_t *clarifae_backend_deepfilternet(void) { return &kDf; }
#endif /* CLARIFAE_WITH_DF */
