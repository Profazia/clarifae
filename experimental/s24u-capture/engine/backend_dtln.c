/* Optional DTLN (breizhn/DTLN, pinned models in backends/dtln).
 * Float/int16 API; both neural stages use NORMALIZED samples.  Per-channel
 * interpreters, fixed zero priming, and sample-clocked queues are intentional.
 * create/destroy run off the audio thread. No fallback is implemented here.
 */
#ifdef CLARIFAE_WITH_DTLN
#include "backend.h"
#include "resampler.h"
#include "tflite/c/c_api.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CLARIFAE_DTLN_MODEL1_PATH
#define CLARIFAE_DTLN_MODEL1_PATH "/vendor/lib64/soundfx/clarifae_models/dtln/model_1.tflite"
#endif
#ifndef CLARIFAE_DTLN_MODEL2_PATH
#define CLARIFAE_DTLN_MODEL2_PATH "/vendor/lib64/soundfx/clarifae_models/dtln/model_2.tflite"
#endif
#define RATE 16000
#define LEN 512
#define HOP 128
#define BINS 257
#define STATES 512
#define QCAP 4096
#define MAX_CHANNELS 8
#define PI 3.14159265358979323846264338327950288

typedef struct {
    TfLiteInterpreter *it;
    TfLiteInterpreterOptions *opts;
    int data_in, state_in, data_out, state_out;
    float state[STATES];
} stage_t;

typedef struct {
    stage_t a, b;
    resampler_t *to16, *from16;
    float input[LEN], ola[LEN];
    float mag[BINS], mask[BINS], estimate[LEN], result[LEN];
    double re[LEN], im[LEN];
    int fill;
    float wetq[QCAP];
    unsigned qread, qcount;
    float *dry;
    unsigned dry_cap, dry_write;
} channel_t;

typedef struct {
    TfLiteModel *m1, *m2;
    channel_t *ch;
    int channels, sample_rate, priming, failed;
    unsigned dry_integer;
    float dry_fraction, strength;
    double tw_re[LEN/2], tw_im[LEN/2];
} dtln_t;

static void explain(char *err, int len, const char *s) {
    if (err && len > 0) snprintf(err, (size_t)len, "%s", s);
}
static int finite_array(const float *v, int n) {
    for (int i=0; i<n; ++i) if (!isfinite(v[i])) return 0;
    return 1;
}
static int shape(const TfLiteTensor *t, int state, int n) {
    static const int ss[4] = {1, 2, 128, 2};
    if (!t || TfLiteTensorType(t) != kTfLiteFloat32 ||
        TfLiteTensorNumDims(t) != (state ? 4 : 3) ||
        TfLiteTensorByteSize(t) != (size_t)(state ? STATES : n)*sizeof(float)) return 0;
    for (int j=0; j<(state ? 4 : 3); ++j)
        if (TfLiteTensorDim(t,j) != (state ? ss[j] : (j==2 ? n : 1))) return 0;
    return 1;
}
static int classify(stage_t *p, int n) {
    if (TfLiteInterpreterGetInputTensorCount(p->it)!=2 ||
        TfLiteInterpreterGetOutputTensorCount(p->it)!=2) return -1;
    p->data_in=p->state_in=p->data_out=p->state_out=-1;
    for (int j=0;j<2;++j) {
        const TfLiteTensor *in=TfLiteInterpreterGetInputTensor(p->it,j);
        const TfLiteTensor *out=TfLiteInterpreterGetOutputTensor(p->it,j);
        if (shape(in,0,n)) p->data_in=j;
        else if (shape(in,1,n)) p->state_in=j;
        else return -1;
        if (shape(out,0,n)) p->data_out=j;
        else if (shape(out,1,n)) p->state_out=j;
        else return -1;
    }
    return (p->data_in<0 || p->state_in<0 || p->data_out<0 || p->state_out<0) ? -1 : 0;
}
static int stage_run(stage_t *p, const float *in, float *out, int n) {
    if (TfLiteTensorCopyFromBuffer(TfLiteInterpreterGetInputTensor(p->it,p->data_in),
                                  in,(size_t)n*sizeof(float))!=kTfLiteOk ||
        TfLiteTensorCopyFromBuffer(TfLiteInterpreterGetInputTensor(p->it,p->state_in),
                                  p->state,sizeof(p->state))!=kTfLiteOk ||
        TfLiteInterpreterInvoke(p->it)!=kTfLiteOk ||
        TfLiteTensorCopyToBuffer(TfLiteInterpreterGetOutputTensor(p->it,p->data_out),
                                out,(size_t)n*sizeof(float))!=kTfLiteOk ||
        TfLiteTensorCopyToBuffer(TfLiteInterpreterGetOutputTensor(p->it,p->state_out),
                                p->state,sizeof(p->state))!=kTfLiteOk) return -EIO;
    return finite_array(out,n) && finite_array(p->state,STATES) ? 0 : -EIO;
}
static int stage_create(stage_t *p, const TfLiteModel *m, int n) {
    p->opts=TfLiteInterpreterOptionsCreate();
    if (!p->opts) return -1;
    TfLiteInterpreterOptionsSetNumThreads(p->opts,1);
    p->it=TfLiteInterpreterCreate(m,p->opts);
    if (!p->it || TfLiteInterpreterAllocateTensors(p->it)!=kTfLiteOk || classify(p,n)) return -1;
    /* Exercise preparation/first-invoke work before the realtime callback.
     * These pinned graphs have explicit LSTM states and NO variable tensors. */
    float in[LEN]={0}, out[LEN];
    for (int k=0;k<2;++k) if (stage_run(p,in,out,n)) return -1;
    memset(p->state,0,sizeof(p->state));
    return 0;
}
static int enqueue(channel_t *c, const float *v, unsigned n) {
    if (n>QCAP-c->qcount) return -EOVERFLOW;
    for (unsigned j=0;j<n;++j) c->wetq[(c->qread+c->qcount+j)%QCAP]=v[j];
    c->qcount+=n;
    return 0;
}
static void fft(dtln_t *s, channel_t *c, int inverse) {
    for (unsigned i=1,j=0;i<LEN;++i) {
        unsigned bit=LEN>>1;
        for (;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if (i<j) {
            double t=c->re[i]; c->re[i]=c->re[j]; c->re[j]=t;
            t=c->im[i]; c->im[i]=c->im[j]; c->im[j]=t;
        }
    }
    for (int len=2;len<=LEN;len*=2) for (int i=0;i<LEN;i+=len)
        for (int k=0;k<len/2;++k) {
            int q=k*LEN/len, b=i+k+len/2, a=i+k;
            double wr=s->tw_re[q], wi=inverse ? -s->tw_im[q] : s->tw_im[q];
            double vr=c->re[b]*wr-c->im[b]*wi, vi=c->re[b]*wi+c->im[b]*wr;
            c->re[b]=c->re[a]-vr; c->im[b]=c->im[a]-vi;
            c->re[a]+=vr; c->im[a]+=vi;
        }
    if (inverse) for (int i=0;i<LEN;++i) {c->re[i]/=LEN;c->im[i]/=LEN;}
}
static int block(dtln_t *s, channel_t *c) {
    for (int i=0;i<LEN;++i) {c->re[i]=c->input[i];c->im[i]=0;}
    fft(s,c,0);
    for (int i=0;i<BINS;++i) c->mag[i]=(float)hypot(c->re[i],c->im[i]);
    if (stage_run(&c->a,c->mag,c->mask,BINS)) return -EIO;
    /* Multiplying complex bins by a real mask preserves the original phase;
     * inverse FFT remains normalized. NEVER scale to int16 before stage 2. */
    for (int i=0;i<BINS;++i) {c->re[i]*=c->mask[i];c->im[i]*=c->mask[i];}
    c->im[0]=c->im[LEN/2]=0;
    for (int i=1;i<LEN/2;++i) {c->re[LEN-i]=c->re[i];c->im[LEN-i]=-c->im[i];}
    fft(s,c,1);
    for (int i=0;i<LEN;++i) c->estimate[i]=(float)c->re[i];
    if (stage_run(&c->b,c->estimate,c->result,LEN)) return -EIO;
    memmove(c->ola,c->ola+HOP,(LEN-HOP)*sizeof(float));
    memset(c->ola+LEN-HOP,0,HOP*sizeof(float));
    for (int i=0;i<LEN;++i) c->ola[i]+=c->result[i];
    if (!finite_array(c->ola,LEN)) return -EIO;
    /* A one-sample SRC call gives exactly the same schedule for every Android
     * callback partition. Max ratio 192k/16k=12; bounded stack scratch. */
    for (int i=0;i<HOP;++i) {
        float out[16], v=c->ola[i]*32768.0f; int produced=0;
        if (!isfinite(v) || rs_process(c->from16,&v,1,out,16,&produced) ||
            enqueue(c,out,(unsigned)produced)) return -EIO;
    }
    return 0;
}
static void reset(void *state) {
    dtln_t *s=state;
    if (!s) return;
    s->failed=0;
    for (int k=0;k<s->channels;++k) {
        channel_t *c=s->ch+k;
        memset(c->input,0,sizeof(c->input)); memset(c->ola,0,sizeof(c->ola));
        memset(c->a.state,0,sizeof(c->a.state)); memset(c->b.state,0,sizeof(c->b.state));
        memset(c->wetq,0,sizeof(c->wetq)); memset(c->dry,0,c->dry_cap*sizeof(float));
        c->fill=0;c->qread=0;c->qcount=(unsigned)s->priming;c->dry_write=0;
        rs_reset(c->to16);rs_reset(c->from16);
    }
}
static void destroy(void *state) {
    dtln_t *s=state;
    if (!s) return;
    if (s->ch) for (int k=0;k<s->channels;++k) {
        channel_t *c=s->ch+k;
        if (c->a.it) TfLiteInterpreterDelete(c->a.it);
        if (c->b.it) TfLiteInterpreterDelete(c->b.it);
        if (c->a.opts) TfLiteInterpreterOptionsDelete(c->a.opts);
        if (c->b.opts) TfLiteInterpreterOptionsDelete(c->b.opts);
        rs_destroy(c->to16);rs_destroy(c->from16);free(c->dry);
    }
    free(s->ch);
    if (s->m1) TfLiteModelDelete(s->m1);
    if (s->m2) TfLiteModelDelete(s->m2);
    free(s);
}
static void *create(int rate, int channels, char *err, int errlen) {
    if (rate<8000 || rate>192000 || channels<1 || channels>MAX_CHANNELS) {
        explain(err,errlen,"DTLN requires 8k..192k Hz and 1..8 channels");return NULL;
    }
    dtln_t *s=calloc(1,sizeof(*s));
    if (!s) {explain(err,errlen,"DTLN state allocation failed");return NULL;}
    s->channels=channels;s->sample_rate=rate;s->strength=1;
    s->priming=rate==RATE ? HOP : (HOP*rate+RATE-1)/RATE+2;
    double delay=s->priming+(double)(LEN-HOP)*rate/RATE;
    s->dry_integer=(unsigned)floor(delay);s->dry_fraction=(float)(delay-s->dry_integer);
    for (int i=0;i<LEN/2;++i) {s->tw_re[i]=cos(2*PI*i/LEN);s->tw_im[i]=-sin(2*PI*i/LEN);}
    s->m1=TfLiteModelCreateFromFile(CLARIFAE_DTLN_MODEL1_PATH);
    s->m2=TfLiteModelCreateFromFile(CLARIFAE_DTLN_MODEL2_PATH);
    if (!s->m1 || !s->m2) {explain(err,errlen,"DTLN model load failed (vendor model paths)");goto fail;}
    s->ch=calloc((size_t)channels,sizeof(*s->ch));
    if (!s->ch) {explain(err,errlen,"DTLN channel allocation failed");goto fail;}
    for (int k=0;k<channels;++k) {
        channel_t *c=s->ch+k;
        c->dry_cap=s->dry_integer+2;c->dry=calloc(c->dry_cap,sizeof(float));
        c->to16=rs_create(rate,RATE);c->from16=rs_create(RATE,rate);
        if (!c->dry || !c->to16 || !c->from16) {explain(err,errlen,"DTLN SRC allocation failed");goto fail;}
        if (stage_create(&c->a,s->m1,BINS) || stage_create(&c->b,s->m2,LEN)) {
            explain(err,errlen,"DTLN tensor contract/allocate/warmup failed");goto fail;
        }
        if (block(s,c)) {explain(err,errlen,"DTLN full-pipeline warmup failed");goto fail;}
    }
    reset(s);explain(err,errlen,"");return s;
fail:
    destroy(s);return NULL;
}
static int process(void *state, float *io, int frames) {
    dtln_t *s=state;
    if (!s || frames<0 || (frames && !io)) return -EINVAL;
    if (!frames) return s->failed ? -EIO : 0;
    if ((size_t)frames>SIZE_MAX/(sizeof(float)*(size_t)s->channels)) return -EINVAL;
    if (s->failed) goto fail;
    for (int i=0;i<frames;++i) for (int k=0;k<s->channels;++k) {
        channel_t *c=s->ch+k;
        size_t at=(size_t)i*s->channels+k;
        float sample=io[at], up[4];int count=0;
        if (!isfinite(sample)) goto fail;
        c->dry[c->dry_write]=sample;
        unsigned a=(c->dry_write+c->dry_cap-s->dry_integer)%c->dry_cap;
        unsigned b=(a+c->dry_cap-1)%c->dry_cap;
        float dry=c->dry[a]+(c->dry[b]-c->dry[a])*s->dry_fraction;
        c->dry_write=(c->dry_write+1)%c->dry_cap;
        sample/=32768.0f;
        if (rs_process(c->to16,&sample,1,up,4,&count)) goto fail;
        for (int j=0;j<count;++j) {
            /* Fill the tail of the rolling 512-sample analysis window. */
            c->input[LEN-HOP+c->fill++]=up[j];
            if (c->fill==HOP) {
                if (block(s,c)) goto fail;
                memmove(c->input,c->input+HOP,(LEN-HOP)*sizeof(float));c->fill=0;
            }
        }
        if (!c->qcount) goto fail; /* A scheduling fault is NOT current dry audio. */
        float wet=c->wetq[c->qread];c->qread=(c->qread+1)%QCAP;--c->qcount;
        float v=s->strength==0 ? dry : dry+s->strength*(wet-dry);
        if (!isfinite(v)) goto fail;
        /* Float transport must not distort strength=0 delayed dry. Wet output
         * is clipped only at the normal PCM range, never for attenuation/VAD. */
        if (s->strength>0) {if(v>32767)v=32767;if(v< -32768)v= -32768;}
        io[at]=v;
    }
    return 0;
fail:
    s->failed=1;
    memset(io,0,(size_t)frames*s->channels*sizeof(float));
    return -EIO;
}
static void params(void *state,float strength,float attenuation,float vad,int hp,int agc) {
    dtln_t *s=state;
    if (!s) return;
    s->strength=!isfinite(strength) ? 1 : (strength<0 ? 0 : (strength>1 ? 1 : strength));
    /* No such controls exist in these pretrained graphs. Do not fake VAD,
     * level floors, highpass or AGC: they change the official model recipe. */
    (void)attenuation;(void)vad;(void)hp;(void)agc;
}
static const clarifae_backend_t api={"dtln",create,process,params,reset,destroy};
const clarifae_backend_t *clarifae_backend_dtln(void) {return &api;}
#endif
