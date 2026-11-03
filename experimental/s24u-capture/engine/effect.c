/*
 * effect.c — Clarifae Android audio effect (capture + playback).
 *
 * One library (symbol "AELI") exposes TWO descriptors so the same DSP cleans
 * audio in both directions:
 *   - INPUT  : a noise-suppression PRE-processing effect on capture streams
 *              (clean your microphone). UUID c1a71fae-0001-...
 *   - OUTPUT : a custom POST-processing effect on playback streams
 *              (clean what you hear). UUID c1a71fae-0002-...
 * AudioFlinger picks one via the UUID in audio_effects.xml (<preprocess> vs
 * <postprocess>); we set the instance "kind" from that UUID.
 *
 * Each pass hands PCM to the selected backend (rnnoise / dtln / deepfilternet).
 * Config is read live from persist.clarifae.* (see config.c) — changes from the
 * WebUI gate/parameter changes apply on the next buffer; model changes build
 * asynchronously without a reboot. Gating is per side:
 * master `enabled` AND (`in_enabled` | `out_enabled`); strength is `strength`
 * for input and `out_strength` for output. When bypassed, audio passes through.
 */
#include "audio_effect.h"
#include "clarifae_stats.h"
#include "clarifae_uuid.h"
#include "backend.h"
#include "config.h"
#include "log.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <float.h>
#include <pthread.h>
#include <stdatomic.h>

#define CLF_KIND_INPUT  0   /* capture pre-processing (clean the microphone) */
#define CLF_KIND_OUTPUT 1   /* playback post-processing (clean what you hear) */

/* Same-context host callbacks are serialized by the effect host. Only mailbox
 * fields cross threads; a backend belongs exclusively to worker OR process. */
typedef struct prepared {
    const clarifae_backend_t *backend;
    void *state;
    int actual_mode, requested_mode;
    uint64_t generation;
    struct prepared *next;
} prepared_t;
typedef struct {
    uint64_t generation;
    int wanted, mode, channels, kind;
    uint32_t rate;
    clarifae_config_t cfg;
} build_request_t;
typedef struct {
    const struct effect_interface_s *itfe; /* ABI: first */
    effect_descriptor_t desc;
    int kind, host_enabled, configured;
    clarifae_config_watch_t config_watch;
    effect_config_t audio_config;
    clarifae_config_t cfg;
    uint64_t processed_frames;
    uint64_t playback_repairs, process_calls;
    float playback_input_peak, playback_output_peak;
    uint32_t sample_rate;
    int channels;
    uint8_t in_format, out_format, in_access, out_access;
    float *fbuf;
    size_t fbuf_cap;
    prepared_t *active; /* host-owned, worker never touches it */
    uint32_t fade_pos, fade_frames, entry_dry_frames;
    pthread_mutex_t mailbox;
    pthread_cond_t wake;
    _Atomic unsigned refs; /* host + detached worker */
    int closing;
    build_request_t request;
    uint64_t attempted_generation;
    prepared_t *ready, *retired; /* mailbox-owned */
} clarifae_ctx_t;

/* ---- descriptors ---- */
static const effect_uuid_t kTypeNs   = CLARIFAE_TYPE_NS_UUID;     /* input type */
static const effect_uuid_t kImpl     = CLARIFAE_IMPL_UUID;        /* input impl */
static const effect_uuid_t kTypeOut  = CLARIFAE_OUT_TYPE_UUID;    /* output type */
static const effect_uuid_t kImplOut  = CLARIFAE_OUT_IMPL_UUID;    /* output impl */

static int uuid_eq(const effect_uuid_t* a, const effect_uuid_t* b) {
    return memcmp(a, b, sizeof(effect_uuid_t)) == 0;
}

/* -1 = not ours, else CLF_KIND_* */
static int kind_for_uuid(const effect_uuid_t* u) {
    if (uuid_eq(u, &kImpl)    || uuid_eq(u, &kTypeNs))  return CLF_KIND_INPUT;
    if (uuid_eq(u, &kImplOut) || uuid_eq(u, &kTypeOut)) return CLF_KIND_OUTPUT;
    return -1;
}

static void fill_descriptor(effect_descriptor_t* d, int kind) {
    memset(d, 0, sizeof(*d));
    d->apiVersion = EFFECT_CONTROL_API_VERSION;
    d->cpuLoad = 20;       /* ~2 MIPS hint */
    d->memoryUsage = 512;  /* KB hint */
    strncpy(d->implementor, "Clarifae Project", EFFECT_STRING_LEN - 1);
    if (kind == CLF_KIND_OUTPUT) {
        d->type  = kTypeOut;
        d->uuid  = kImplOut;
        d->flags = EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_LAST |
                   EFFECT_FLAG_INPUT_DIRECT | EFFECT_FLAG_OUTPUT_DIRECT;
        strncpy(d->name, "Clarifae Out", EFFECT_STRING_LEN - 1);
    } else {
        d->type  = kTypeNs;
        d->uuid  = kImpl;
        d->flags = EFFECT_FLAG_TYPE_PRE_PROC | EFFECT_FLAG_INSERT_FIRST |
                   EFFECT_FLAG_INPUT_DIRECT | EFFECT_FLAG_OUTPUT_DIRECT;
        strncpy(d->name, "Clarifae NS", EFFECT_STRING_LEN - 1);
    }
}

/* ---- asynchronous lifecycle: no model operation under mailbox ---- */
static int effective(const clarifae_ctx_t *c) {
    return c->configured && c->host_enabled && c->cfg.enabled &&
           (c->kind ? c->cfg.out_enabled : c->cfg.in_enabled);
}
static void apply_params(prepared_t *p, const clarifae_config_t *cfg, int kind) {
    p->backend->set_params(p->state, (kind ? cfg->out_strength : cfg->strength)/100.f,
                          cfg->atten_db, cfg->vad/100.f, kind ? 0 : cfg->highpass, kind ? 0 : cfg->autogain);
}
static void retire_locked(clarifae_ctx_t *c, prepared_t *p) {
    if (p) { p->next=c->retired; c->retired=p; }
}
static void destroy_prepared(prepared_t *p) {
    while (p) { prepared_t *next=p->next; p->backend->destroy(p->state); free(p); p=next; }
}
static void context_unref(clarifae_ctx_t *c) {
    if (atomic_fetch_sub_explicit(&c->refs,1,memory_order_acq_rel)!=1) return;
    pthread_cond_destroy(&c->wake);
    pthread_mutex_destroy(&c->mailbox);
    free(c->fbuf);
    free(c);
#ifdef CLARIFAE_ASYNC_TEST
    extern void test_context_freed(void);
    test_context_freed();
#endif
}
/* Host only. Mode-only changes keep a compatible previous backend alive.
 * Discontinuities retire it immediately and prepare a fresh state, never reset
 * a model in any callback. At most active+ready+one build can exist; retire
 * uses those same nodes, so repeated requests cannot grow a job queue. */
static void request_update(clarifae_ctx_t *c, int discontinuity) {
    int wanted=effective(c);
    pthread_mutex_lock(&c->mailbox);
    int changed=discontinuity || c->request.wanted!=wanted ||
                c->request.mode!=c->cfg.mode || c->request.rate!=c->sample_rate ||
                c->request.channels!=c->channels;
    if (changed) {
        ++c->request.generation;
        retire_locked(c,c->ready); c->ready=NULL;
        if (discontinuity || !wanted) { retire_locked(c,c->active); c->active=NULL; }
        c->request.wanted=wanted; c->request.mode=c->cfg.mode;
        c->request.rate=c->sample_rate; c->request.channels=c->channels;
        c->request.kind=c->kind;
        /* Returning to an already-running genuine mode needs no duplicate build. */
        if (c->active && c->active->actual_mode==c->cfg.mode)
            c->attempted_generation=c->request.generation;
    }
    c->request.cfg=c->cfg;
    if (changed) pthread_cond_signal(&c->wake);
    pthread_mutex_unlock(&c->mailbox);
}
static void refresh_config(clarifae_ctx_t *c) {
    if (clarifae_config_changed(&c->config_watch)) {
        /* Watch BEFORE load: a concurrent property update is not consumed. */
        clarifae_config_load(&c->cfg);
        request_update(c,0);
        if (c->active) apply_params(c->active,&c->cfg,c->kind);
    }
}
static prepared_t *prepare(const build_request_t *r) {
    char err[160]={0};
    const clarifae_backend_t *b=clarifae_backend_for_mode(r->mode);
    void *s=b->create((int)r->rate,r->channels,err,sizeof(err));
    if (!s && b!=clarifae_backend_rnnoise()) {
        CLARIFAE_LOGW("requested mode=%d create failed (%s); actual fallback=rnnoise",r->mode,err);
        b=clarifae_backend_rnnoise(); s=b->create((int)r->rate,r->channels,err,sizeof(err));
    }
    if (!s) { CLARIFAE_LOGE("backend build failed: %s",err); return NULL; }
    prepared_t *p=calloc(1,sizeof(*p));
    if (!p) { b->destroy(s); return NULL; }
    p->backend=b; p->state=s; p->generation=r->generation; p->requested_mode=r->mode;
    p->actual_mode=!strcmp(b->name,"dtln")?2:(!strcmp(b->name,"deepfilternet")?3:1);
    apply_params(p,&r->cfg,r->kind);
    /* 60ms bounded zero prime exceeds current 40ms maximum pipeline delay.
     * Count neither priming nor failed inference as live successful frames. */
    float zeros[480*8];
    uint32_t remain=(r->rate*60u+999u)/1000u;
    while (remain) {
        uint32_t n=remain>480?480:remain;
        memset(zeros,0,(size_t)n*(size_t)r->channels*sizeof(float));
        if (b->process(s,zeros,(int)n)) { destroy_prepared(p); return NULL; }
        for (size_t i=0;i<(size_t)n*(size_t)r->channels;i++)
            if (!isfinite(zeros[i])) { destroy_prepared(p); return NULL; }
        remain-=n;
    }
    return p;
}
static void *model_worker(void *opaque) {
    clarifae_ctx_t *c=opaque;
    for (;;) {
        pthread_mutex_lock(&c->mailbox);
        while (!c->closing && !c->retired &&
               !(c->request.wanted && c->attempted_generation!=c->request.generation))
            pthread_cond_wait(&c->wake,&c->mailbox);
        prepared_t *retired=c->retired; c->retired=NULL;
        int closing=c->closing;
        build_request_t job=c->request;
        int build=!closing && job.wanted && c->attempted_generation!=job.generation;
        if (build) c->attempted_generation=job.generation;
        pthread_mutex_unlock(&c->mailbox);
        destroy_prepared(retired);
        if (closing) break;
        if (!build) continue;
        prepared_t *made=prepare(&job);
        pthread_mutex_lock(&c->mailbox);
        prepared_t *old=NULL;
        if (made && !c->closing && c->request.wanted &&
            c->request.generation==job.generation) {
            old=c->ready; c->ready=made; made=NULL;
        }
        pthread_mutex_unlock(&c->mailbox);
        destroy_prepared(old); destroy_prepared(made);
    }
    context_unref(c);
    return NULL;
}
/* Mailbox lock is held ONLY for pointer/snapshot moves, not build, destroy,
 * reset, params or inference. Audio cannot queue behind a constructor. */
static void adopt_ready(clarifae_ctx_t *c) {
    pthread_mutex_lock(&c->mailbox);
    prepared_t *p=c->ready;
    if (p) {
        c->ready=NULL;
        if (c->request.wanted && p->generation==c->request.generation) {
            retire_locked(c,c->active); c->active=p;
            c->fade_pos=0; c->fade_frames=(c->sample_rate*15u+999u)/1000u;
            /* The first live signal still traverses fixed model latency after
             * zero priming. Keep current dry for 60ms, THEN ramp over 15ms;
             * otherwise a short fade to the delayed zeros creates a mute dip. */
            c->entry_dry_frames=(c->sample_rate*60u+999u)/1000u;
        } else retire_locked(c,p);
        pthread_cond_signal(&c->wake);
    }
    pthread_mutex_unlock(&c->mailbox);
    if (p && c->active==p) apply_params(p,&c->cfg,c->kind);
}

static int ensure_fbuf(clarifae_ctx_t* c, size_t need) {
    if (need <= c->fbuf_cap) return 0;
    float* nb = (float*)realloc(c->fbuf, need * sizeof(float));
    if (!nb) return -ENOMEM;
    c->fbuf = nb;
    c->fbuf_cap = need;
    return 0;
}

/* ---- effect interface callbacks ---- */
static int32_t ce_process_block(effect_handle_t self, audio_buffer_t *in, audio_buffer_t *out) {
    clarifae_ctx_t *c=(clarifae_ctx_t*)self;
    size_t frames=in->frameCount, n=frames*(size_t)c->channels;
    /* Valid output buffers always receive a complete signal. Finite mixer
     * headroom is preserved in dry bypass; corrupt playback samples are
     * contained locally instead of abandoning the entire output callback. */
    int corrupt_playback=0;
    if(c->in_format==AUDIO_FORMAT_PCM_FLOAT) {
        for(size_t i=0;i<n;i++) {
            float v=in->f32[i];
            if(!isfinite(v)) {
                if(!c->kind)return -EINVAL;
                corrupt_playback=1;c->playback_repairs++;
            } else {
                if(!c->kind && fabsf(v)>1.f)return -EINVAL;
                if(c->kind && fabsf(v)>c->playback_input_peak)c->playback_input_peak=fabsf(v);
            }
        }
    }
    if(corrupt_playback && c->active)request_update(c,1);
    int wet=effective(c) && c->active;
    if (wet) {
        for (size_t i=0;i<n;i++) {
            float v=c->in_format==AUDIO_FORMAT_PCM_FLOAT ? in->f32[i] : 0.f;
            if(c->in_format==AUDIO_FORMAT_PCM_FLOAT) {
                if(!isfinite(v))v=0.f;
                if(v>1.f)v=1.f;else if(v< -1.f)v=-1.f;
                c->fbuf[i]=v*32768.f;
            } else c->fbuf[i]=in->s16[i];
        }
        int rc=c->active->backend->process(c->active->state,c->fbuf,(int)frames);
        if (!rc) for (size_t i=0;i<n;i++) if (!isfinite(c->fbuf[i])) { rc=-EINVAL; break; }
        if (rc) {
            /* A failed state cannot leak stale output: raw this block, prepare a
             * clean replacement off-thread. No live-success counter increment. */
            request_update(c,1); wet=0;
        } else c->processed_frames+=frames;
    }
    /* Input is read before output for in-place processing/accumulation. Entry
     * ramp uses current dry audio; fixed backend delay is intentionally NOT
     * claimed bit-exact or perceptually seamless. */
    for (size_t frame=0;frame<frames;frame++) {
        float a=wet ? (c->fade_pos>=c->fade_frames ? 1.f : (float)c->fade_pos/(float)c->fade_frames) : 0.f;
        if (wet && c->entry_dry_frames) { a=0.f; --c->entry_dry_frames; }
        else if (wet && c->fade_pos<c->fade_frames) ++c->fade_pos;
        for (int channel=0;channel<c->channels;channel++) {
            size_t i=frame*(size_t)c->channels+(size_t)channel;
            if (!wet || a==0.f) {
                if (c->in_format==AUDIO_FORMAT_PCM_FLOAT) {
                    float v=in->f32[i];
                    if(c->kind && !isfinite(v))v=0.f;
                    if(c->out_access==EFFECT_BUFFER_ACCESS_ACCUMULATE) {
                        float old=out->f32[i];
                        if(c->kind && !isfinite(old)){old=0.f;c->playback_repairs++;}
                        double sum=(double)old+v;
                        out->f32[i]=c->kind ? (float)fmax(-(double)FLT_MAX,fmin((double)FLT_MAX,sum)) : old+v;
                    } else out->f32[i]=v;
                } else {
                    int32_t v=in->s16[i];
                    if (c->out_access==EFFECT_BUFFER_ACCESS_ACCUMULATE) v+=out->s16[i];
                    out->s16[i]=(int16_t)(v<-32768?-32768:(v>32767?32767:v));
                }
            } else {
                double dry=c->in_format==AUDIO_FORMAT_PCM_FLOAT ? (double)in->f32[i]*32768.0 : in->s16[i];
                if(!isfinite(dry))dry=0;
                double mixed=(1.0-a)*dry+a*c->fbuf[i];
                float v=(float)fmax(-32768.0,fmin(32767.0,mixed)); /* clamp before integer conversion */
                if (c->out_format==AUDIO_FORMAT_PCM_FLOAT) {
                    v/=32768.f;
                    if (c->out_access==EFFECT_BUFFER_ACCESS_ACCUMULATE) {
                        float old=out->f32[i];
                        if(c->kind && !isfinite(old)){old=0.f;c->playback_repairs++;}
                        double sum=(double)old+v;
                        out->f32[i]=c->kind ? (float)fmax(-(double)FLT_MAX,fmin((double)FLT_MAX,sum)) : old+v;
                    } else out->f32[i]=v;
                } else {
                    int32_t value=(int32_t)lrintf(v);
                    if (c->out_access==EFFECT_BUFFER_ACCESS_ACCUMULATE) value+=out->s16[i];
                    out->s16[i]=(int16_t)(value<-32768?-32768:(value>32767?32767:value));
                }
            }
        }
    }
    c->process_calls++;
    if(c->kind && c->out_format==AUDIO_FORMAT_PCM_FLOAT)
        for(size_t i=0;i<n;i++)if(isfinite(out->f32[i]) && fabsf(out->f32[i])>c->playback_output_peak)c->playback_output_peak=fabsf(out->f32[i]);
    return c->host_enabled ? 0 : -ENODATA;
}

static int32_t ce_process_reverse(effect_handle_t self,
                                  audio_buffer_t* in, audio_buffer_t* out) {
    (void)self; (void)in; (void)out;
    return -ENOSYS;   /* no far-end reference path */
}

/* Split blocks to bound scratch memory and RNNoise's streaming queues. */
static int32_t ce_process(effect_handle_t self, audio_buffer_t* in, audio_buffer_t* out) {
    clarifae_ctx_t* c = (clarifae_ctx_t*)self;
    if (!c || !c->configured) return -EINVAL;
    if (!in) in = &c->audio_config.inputCfg.buffer;
    if (!out) out = &c->audio_config.outputCfg.buffer;
    if (!in->raw || !out->raw || out->frameCount < in->frameCount) return -EINVAL;
    if (in->frameCount > SIZE_MAX / ((size_t)c->channels * sizeof(float))) return -EINVAL;
    /* One backend per whole callback, including callbacks split into chunks. */
    refresh_config(c);
    adopt_ready(c);
    size_t off = 0;
    int32_t status = 0;
    while (off < in->frameCount) {
        size_t frames = in->frameCount - off;
        if (frames > 2048) frames = 2048;
        audio_buffer_t ib = { .frameCount = frames }, ob = { .frameCount = frames };
        size_t insz = c->in_format == AUDIO_FORMAT_PCM_FLOAT ? sizeof(float) : sizeof(int16_t);
        size_t outsz = c->out_format == AUDIO_FORMAT_PCM_FLOAT ? sizeof(float) : sizeof(int16_t);
        ib.raw = (char*)in->raw + off * c->channels * insz;
        ob.raw = (char*)out->raw + off * c->channels * outsz;
        status = ce_process_block(self, &ib, &ob);
        if (status && status != -ENODATA) return status;
        off += frames;
    }
    return status;
}

static int channels_for_mask(uint32_t mask) {
    /* Android index masks reserve the top two bits for representation. */
    return __builtin_popcount(mask & 0x3fffffffu);
}

static int configure(clarifae_ctx_t* c, const effect_config_t* requested) {
    effect_config_t ec = c->audio_config;
    const buffer_config_t* req[2] = {&requested->inputCfg, &requested->outputCfg};
    buffer_config_t* dst[2] = {&ec.inputCfg, &ec.outputCfg};
    for (int i = 0; i < 2; ++i) {
        uint16_t mask = req[i]->mask;
        if (mask & ~EFFECT_CONFIG_ALL) return -EINVAL;
        if (mask & EFFECT_CONFIG_BUFFER) {
            dst[i]->buffer = req[i]->buffer;
            dst[i]->bufferProvider = req[i]->bufferProvider;
        }
        if (mask & EFFECT_CONFIG_SMP_RATE) dst[i]->samplingRate = req[i]->samplingRate;
        if (mask & EFFECT_CONFIG_CHANNELS) dst[i]->channels = req[i]->channels;
        if (mask & EFFECT_CONFIG_FORMAT) dst[i]->format = req[i]->format;
        if (mask & EFFECT_CONFIG_ACC_MODE) dst[i]->accessMode = req[i]->accessMode;
        dst[i]->mask = EFFECT_CONFIG_ALL;
    }
    int ch = channels_for_mask(ec.inputCfg.channels);
    if (ch < 1 || ch > 8 || ch != channels_for_mask(ec.outputCfg.channels) ||
        ec.inputCfg.samplingRate < 8000 || ec.inputCfg.samplingRate > 192000 ||
        ec.inputCfg.samplingRate != ec.outputCfg.samplingRate ||
        (ec.inputCfg.format != AUDIO_FORMAT_PCM_16_BIT && ec.inputCfg.format != AUDIO_FORMAT_PCM_FLOAT) ||
        ec.inputCfg.format != ec.outputCfg.format ||
        ec.inputCfg.accessMode != EFFECT_BUFFER_ACCESS_READ ||
        (ec.outputCfg.accessMode != EFFECT_BUFFER_ACCESS_WRITE &&
         ec.outputCfg.accessMode != EFFECT_BUFFER_ACCESS_ACCUMULATE)) return -EINVAL;
    /* Provider callbacks are not supported; refuse instead of dereferencing NULL. */
    if (ec.inputCfg.bufferProvider.getBuffer || ec.outputCfg.bufferProvider.getBuffer) return -ENOSYS;
    int discontinuity=!c->configured || c->sample_rate!=ec.inputCfg.samplingRate ||
                      c->channels!=ch || c->in_format!=ec.inputCfg.format ||
                      c->out_format!=ec.outputCfg.format;
    if (ensure_fbuf(c, 2048u * (size_t)ch)) return -ENOMEM;
    c->audio_config = ec;
    c->sample_rate = ec.inputCfg.samplingRate;
    c->channels = ch;
    c->in_format = ec.inputCfg.format;
    c->out_format = ec.outputCfg.format;
    c->in_access = ec.inputCfg.accessMode;
    c->out_access = ec.outputCfg.accessMode;
    /* Snapshot before loading: never consume a disable arriving during load. */
    clarifae_config_changed(&c->config_watch);
    clarifae_config_load(&c->cfg);
    c->configured = 1;
    request_update(c,discontinuity);
    return 0;
}

static int reply_status(uint32_t* size, void* reply, int status) {
    if (!size || !reply || *size < sizeof(int32_t)) return -EINVAL;
    *(int32_t*)reply = status;
    *size = sizeof(int32_t);
    return 0;
}

static int32_t ce_command(effect_handle_t self, uint32_t cmd, uint32_t cmdSize,
                          void* pCmd, uint32_t* replySize, void* pReply) {
    clarifae_ctx_t* c = (clarifae_ctx_t*)self;
    if (!c) return -EINVAL;
    switch (cmd) {
    case EFFECT_CMD_INIT:
        if (cmdSize || reply_status(replySize, pReply, 0)) return -EINVAL;
        c->host_enabled = 0;
        c->processed_frames = 0;
        request_update(c,1);
        return 0;
    case EFFECT_CMD_SET_CONFIG: {
        if (!pCmd || cmdSize != sizeof(effect_config_t) ||
            !replySize || !pReply || *replySize < sizeof(int32_t)) return -EINVAL;
        return reply_status(replySize, pReply, configure(c, (const effect_config_t*)pCmd));
    }
    case EFFECT_CMD_GET_CONFIG:
        if (cmdSize || !replySize || !pReply || *replySize < sizeof(effect_config_t)) return -EINVAL;
        *(effect_config_t*)pReply = c->audio_config;
        *replySize = sizeof(effect_config_t);
        return 0;
    case EFFECT_CMD_RESET:
        if (cmdSize) return -EINVAL;
        c->processed_frames = 0;
        refresh_config(c);
        request_update(c,1);
        return 0;
    case EFFECT_CMD_ENABLE:
    case EFFECT_CMD_DISABLE:
        if (cmdSize || reply_status(replySize, pReply, 0)) return -EINVAL;
        int was_enabled=c->host_enabled;
        c->host_enabled = cmd == EFFECT_CMD_ENABLE;
        refresh_config(c);
        if (!c->host_enabled) c->processed_frames=0;
        request_update(c,was_enabled!=c->host_enabled);
        return 0;
    case EFFECT_CMD_SET_PARAM:
    case EFFECT_CMD_SET_PARAM_DEFERRED:
    case EFFECT_CMD_SET_PARAM_COMMIT:
    case EFFECT_CMD_GET_PARAM:
        /* No standardized NS level/type emulation: settings are property based.
         * Never return a fabricated successful/empty effect_param_t. */
        return -EINVAL;
    case EFFECT_CMD_SET_DEVICE:
    case EFFECT_CMD_SET_INPUT_DEVICE:
    case EFFECT_CMD_SET_AUDIO_MODE:
    case EFFECT_CMD_SET_AUDIO_SOURCE:
        return (pCmd && cmdSize == sizeof(uint32_t)) ? 0 : -EINVAL;
    case EFFECT_CMD_SET_VOLUME:
        /* VOLUME_NONE: no reply and no volume-controller capability. */
        return (pCmd && cmdSize && cmdSize % sizeof(uint32_t) == 0) ? 0 : -EINVAL;
    case EFFECT_CMD_OFFLOAD:
        /* Software-only; cannot run in the DSP offload path. */
        return reply_status(replySize, pReply, -EINVAL);
    case CLF_CMD_GET_STATS: {
        if (cmdSize || !replySize || !pReply || *replySize < sizeof(clarifae_private_stats_t)) return -EINVAL;
        /* Observation only: never swap a backend between the wrapper
         * before/after counters. Adoption occurs only at process entry. */
        clarifae_private_stats_t stats = {
            .size=sizeof(stats), .version=CLF_STATS_VERSION,
            .processed_frames=c->processed_frames, .sample_rate=c->sample_rate,
            .channels=(uint32_t)c->channels, .kind=c->kind, .host_enabled=c->host_enabled,
            .configured=c->configured, .enabled=c->cfg.enabled,
            .in_enabled=c->cfg.in_enabled, .out_enabled=c->cfg.out_enabled,
            .strength=c->kind ? c->cfg.out_strength : c->cfg.strength, .mode=c->active ? c->active->actual_mode : 0,
            .requested_mode=c->cfg.mode,
            .pending_mode=effective(c) && (!c->active || c->active->actual_mode!=c->cfg.mode)

        };
        memcpy(pReply, &stats, sizeof(stats)); *replySize=sizeof(stats); return 0;
    }
    case EFFECT_CMD_DUMP:
        if (!pCmd || cmdSize != sizeof(int)) return -EINVAL;
        dprintf(*(int*)pCmd, "Clarifae %s backend=%s host_enabled=%d configured=%d "
                "rate=%u channels=%d processed_frames=%llu strength=%d active_mode=%d requested_mode=%d\n",
                c->kind ? "output" : "input", c->active ? c->active->backend->name : "none",
                c->host_enabled, c->configured, c->sample_rate, c->channels,
                (unsigned long long)c->processed_frames,
                c->kind ? c->cfg.out_strength : c->cfg.strength,c->active ? c->active->actual_mode : 0,c->cfg.mode);
        if(c->kind)dprintf(*(int*)pCmd,"Clarifae playback_signal process_calls=%llu repaired_samples=%llu input_peak=%g output_peak=%g\n",(unsigned long long)c->process_calls,(unsigned long long)c->playback_repairs,c->playback_input_peak,c->playback_output_peak);
        return 0;
    default: return -EINVAL;
    }
}

static int32_t ce_get_descriptor(effect_handle_t self, effect_descriptor_t* d) {
    clarifae_ctx_t* c = (clarifae_ctx_t*)self;
    if (!c || !d) return -EINVAL;
    *d = c->desc;
    return 0;
}

static const struct effect_interface_s kInterface = {
    .process         = ce_process,
    .command         = ce_command,
    .get_descriptor  = ce_get_descriptor,
    .process_reverse = ce_process_reverse,
};

/* ---- library interface ---- */
static int32_t lib_create(const effect_uuid_t* uuid, int32_t sessionId,
                          int32_t ioId, effect_handle_t* pHandle) {
    (void)sessionId; (void)ioId;
    if (!pHandle || !uuid) return -EINVAL;
    int kind = kind_for_uuid(uuid);
    if (kind < 0) return -EINVAL;

    clarifae_ctx_t* c = (clarifae_ctx_t*)calloc(1, sizeof(*c));
    if (!c) return -ENOMEM;
    c->itfe = &kInterface;
    c->kind = kind;
    fill_descriptor(&c->desc, kind);
    clarifae_config_changed(&c->config_watch);
    clarifae_config_load(&c->cfg);
    uint32_t mask = kind == CLF_KIND_INPUT ? AUDIO_CHANNEL_IN_MONO : AUDIO_CHANNEL_OUT_STEREO;
    c->audio_config.inputCfg.samplingRate = c->audio_config.outputCfg.samplingRate = 48000;
    c->audio_config.inputCfg.channels = c->audio_config.outputCfg.channels = mask;
    c->audio_config.inputCfg.format = c->audio_config.outputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
    c->audio_config.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    c->audio_config.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
    c->audio_config.inputCfg.mask = c->audio_config.outputCfg.mask = EFFECT_CONFIG_ALL;

    int rc=pthread_mutex_init(&c->mailbox,NULL);
    if (rc) { free(c); return -rc; }
    rc=pthread_cond_init(&c->wake,NULL);
    if (rc) { pthread_mutex_destroy(&c->mailbox); free(c); return -rc; }
    atomic_init(&c->refs,2);
    pthread_attr_t attr;
    rc=pthread_attr_init(&attr);
    if (rc) { atomic_store(&c->refs,1); context_unref(c); return -rc; }
    rc=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    pthread_t worker;
    if (!rc) rc=pthread_create(&worker,&attr,model_worker,c);
    pthread_attr_destroy(&attr);
    if (rc) { atomic_store(&c->refs,1); context_unref(c); return -rc; }
    *pHandle = (effect_handle_t)c;
    CLARIFAE_LOGI("effect created: %s (session %d)",
                  kind == CLF_KIND_OUTPUT ? "output" : "input", sessionId);
    return 0;
}

static int32_t lib_release(effect_handle_t handle) {
    clarifae_ctx_t* c = (clarifae_ctx_t*)handle;
    if (!c) return -EINVAL;
    /* The detached worker keeps its own context reference through any slow
     * constructor. NODELETE on Android pins callback code after HAL dlclose. */
    pthread_mutex_lock(&c->mailbox);
    c->closing=1; ++c->request.generation; c->request.wanted=0;
    retire_locked(c,c->active); c->active=NULL;
    retire_locked(c,c->ready); c->ready=NULL;
    pthread_cond_signal(&c->wake);
    pthread_mutex_unlock(&c->mailbox);
    context_unref(c);
    return 0;
}

static int32_t lib_get_descriptor(const effect_uuid_t* uuid,
                                  effect_descriptor_t* d) {
    if (!uuid || !d) return -EINVAL;
    int kind = kind_for_uuid(uuid);
    if (kind < 0) return -EINVAL;
    fill_descriptor(d, kind);
    return 0;
}

__attribute__((visibility("default")))
audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
    .tag         = AUDIO_EFFECT_LIBRARY_TAG,
    .version     = EFFECT_LIBRARY_API_VERSION,
    .name        = "Clarifae",
    .implementor = "Clarifae Project",
    .create_effect   = lib_create,
    .release_effect  = lib_release,
    .get_descriptor  = lib_get_descriptor,
};
