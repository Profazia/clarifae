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
 * WebUI apply on the next buffer without a reboot. Gating is per side:
 * master `enabled` AND (`in_enabled` | `out_enabled`); strength is `strength`
 * for input and `out_strength` for output. When bypassed, audio passes through.
 */
#include "audio_effect.h"
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

#define CLF_KIND_INPUT  0   /* capture pre-processing (clean the microphone) */
#define CLF_KIND_OUTPUT 1   /* playback post-processing (clean what you hear) */

typedef struct {
    const struct effect_interface_s* itfe;  /* MUST be first: handle aliases this */
    effect_descriptor_t desc;
    int      kind;           /* CLF_KIND_INPUT | CLF_KIND_OUTPUT */

    int      host_enabled;   /* EFFECT_CMD_ENABLE/DISABLE */
    int      configured;
    clarifae_config_watch_t config_watch;
    effect_config_t audio_config;
    uint64_t processed_frames;
    int bypassed;
    uint32_t sample_rate;
    int      channels;
    uint8_t  in_format, out_format;
    uint8_t  in_access, out_access;

    const clarifae_backend_t* backend;
    void*    bstate;
    int      cur_mode;

    clarifae_config_t cfg;

    float*   fbuf;           /* interleaved float scratch [frames*channels] */
    size_t   fbuf_cap;       /* capacity in floats */
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
        d->flags = EFFECT_FLAG_TYPE_POST_PROC | EFFECT_FLAG_INSERT_LAST |
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

/* ---- backend lifecycle ---- */
static void backend_close(clarifae_ctx_t* c) {
    if (c->backend && c->bstate) c->backend->destroy(c->bstate);
    c->backend = NULL;
    c->bstate = NULL;
}

static void backend_apply_params(clarifae_ctx_t* c) {
    if (c->backend && c->bstate) {
        int strength = (c->kind == CLF_KIND_OUTPUT) ? c->cfg.out_strength
                                                     : c->cfg.strength;
        c->backend->set_params(c->bstate,
                               (float)strength / 100.0f,
                               (float)c->cfg.atten_db,
                               (float)c->cfg.vad / 100.0f,
                               c->cfg.highpass, c->cfg.autogain);
    }
}

static int backend_open(clarifae_ctx_t* c) {
    backend_close(c);
    if (c->sample_rate == 0 || c->channels <= 0) return -EINVAL;

    char err[160] = {0};
    const clarifae_backend_t* b = clarifae_backend_for_mode(c->cfg.mode);
    void* st = b->create((int)c->sample_rate, c->channels, err, sizeof(err));
    if (!st && b != clarifae_backend_rnnoise()) {
        CLARIFAE_LOGW("backend '%s' create failed (%s); falling back to rnnoise",
                      b->name, err);
        b = clarifae_backend_rnnoise();
        st = b->create((int)c->sample_rate, c->channels, err, sizeof(err));
    }
    if (!st) {
        CLARIFAE_LOGE("backend create failed: %s", err);
        return -ENODEV;
    }
    c->backend = b;
    c->bstate = st;
    c->cur_mode = c->cfg.mode;
    backend_apply_params(c);
    CLARIFAE_LOGI("active backend=%s mode=%d rate=%u ch=%d", b->name, c->cfg.mode,
                  c->sample_rate, c->channels);
    return 0;
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
static int32_t ce_process_block(effect_handle_t self,
                          audio_buffer_t* in, audio_buffer_t* out) {
    clarifae_ctx_t* c = (clarifae_ctx_t*)self;
    if (!c || !in || !out || !in->raw || !out->raw) return -EINVAL;

    if (!c->configured) return -EINVAL;
    size_t frames = in->frameCount;
    if (out->frameCount < frames) frames = out->frameCount;
    int ch = c->channels > 0 ? c->channels : 1;
    if (frames > 2048 || frames > SIZE_MAX / ((size_t)ch * sizeof(float)))
        return -EINVAL; /* wrapper below splits large buffers */

    /* live config refresh (per-instance property snapshots) */
    if (clarifae_config_changed(&c->config_watch)) {
        int prev_mode = c->cfg.mode;
        clarifae_config_load(&c->cfg);   /* also updates clarifae_log_level */
        if (c->configured) {
            if (!c->backend || (c->cfg.mode != prev_mode &&
                clarifae_backend_for_mode(c->cfg.mode) != c->backend)) {
                /* Retain live legacy mode switching. This may allocate/load
                 * optional runtimes on the audio thread; it is NOT RT-safe. */
                backend_open(c);
            } else {
                backend_apply_params(c);
            }
        }
    }

    int side_enabled = (c->kind == CLF_KIND_OUTPUT) ? c->cfg.out_enabled
                                                     : c->cfg.in_enabled;
    int bypass = (!c->host_enabled) || (!c->cfg.enabled) || (!side_enabled) ||
                 (!c->configured) || (!c->backend) || (!c->bstate);

    if (bypass && !c->bypassed && c->backend && c->bstate) {
        c->backend->reset(c->bstate);
        c->processed_frames = 0;
    }
    c->bypassed = bypass;

    /* Validate before bypass as well: invalid float PCM must not reach a
     * backend, or leak NaN/Inf through an otherwise bit-exact copy. Float
     * accumulation may exceed unity but its destination must be finite. */
    size_t n = frames * (size_t)ch;
    if (c->in_format == AUDIO_FORMAT_PCM_FLOAT) {
        for (size_t i = 0; i < n; i++) {
            if (!isfinite(in->f32[i]) || fabsf(in->f32[i]) > 1.0f ||
                (c->out_access == EFFECT_BUFFER_ACCESS_ACCUMULATE &&
                 !isfinite(out->f32[i]))) return -EINVAL;
        }
    }

    /* ---- bypass: copy input to output (respecting accumulate) ---- */
    if (bypass) {
        if (c->in_format == AUDIO_FORMAT_PCM_FLOAT) {
            if (c->out_access == EFFECT_BUFFER_ACCESS_ACCUMULATE)
                for (size_t i = 0; i < n; i++) out->f32[i] += in->f32[i];
            else
                memmove(out->f32, in->f32, n * sizeof(float));
        } else {
            if (c->out_access == EFFECT_BUFFER_ACCESS_ACCUMULATE)
                for (size_t i = 0; i < n; i++) {
                    int32_t v = out->s16[i] + in->s16[i];
                    out->s16[i] = (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
                }
            else
                memmove(out->s16, in->s16, n * sizeof(int16_t));
        }
        return c->host_enabled ? 0 : -ENODATA;
    }

    if (!c->processed_frames)
        CLARIFAE_LOGI("processing %s backend=%s strength=%d rate=%u ch=%d",
            c->kind ? "output" : "input", c->backend->name,
            c->kind ? c->cfg.out_strength : c->cfg.strength, c->sample_rate, ch);
    /* SET_CONFIG reserves the maximum chunk; do not allocate scratch here. */
    if (n > c->fbuf_cap) return -EINVAL;

    /* input -> float (int16 range) */
    if (c->in_format == AUDIO_FORMAT_PCM_FLOAT) {
        for (size_t i = 0; i < n; i++) c->fbuf[i] = in->f32[i] * 32768.0f;
    } else {
        for (size_t i = 0; i < n; i++) c->fbuf[i] = (float)in->s16[i];
    }

    if (c->backend->process(c->bstate, c->fbuf, (int)frames)) return -EINVAL;
    for (size_t i = 0; i < n; i++)
        if (!isfinite(c->fbuf[i])) return -EINVAL;

    /* float -> output */
    if (c->out_format == AUDIO_FORMAT_PCM_FLOAT) {
        for (size_t i = 0; i < n; i++) {
            float v = c->fbuf[i] / 32768.0f;
            if (v < -1.0f) v = -1.0f; else if (v > 1.0f) v = 1.0f;
            if (c->out_access == EFFECT_BUFFER_ACCESS_ACCUMULATE) out->f32[i] += v;
            else out->f32[i] = v;
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            /* Clamp BEFORE rounding/conversion, including huge finite output
             * from an optional backend (long is only 32 bits on armeabi-v7a). */
            float fv = c->fbuf[i];
            if (fv < -32768.0f) fv = -32768.0f;
            else if (fv > 32767.0f) fv = 32767.0f;
            int32_t v = (int32_t)lrintf(fv);
            if (c->out_access == EFFECT_BUFFER_ACCESS_ACCUMULATE) {
                int32_t a = out->s16[i] + v;
                out->s16[i] = (int16_t)(a < -32768 ? -32768 : (a > 32767 ? 32767 : a));
            } else {
                out->s16[i] = (int16_t)v;
            }
        }
    }
    c->processed_frames += frames;
    return 0;
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
    /* Positional (0) and index (2) are the defined representations. */
    uint32_t representation = mask >> 30;
    if (representation != 0 && representation != 2) return 0;
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
    if (ec.inputCfg.bufferProvider.getBuffer || ec.inputCfg.bufferProvider.releaseBuffer ||
        ec.outputCfg.bufferProvider.getBuffer || ec.outputCfg.bufferProvider.releaseBuffer) return -ENOSYS;
    if (ensure_fbuf(c, 2048u * (size_t)ch)) return -ENOMEM;
    c->audio_config = ec;
    c->sample_rate = ec.inputCfg.samplingRate;
    c->channels = ch;
    c->in_format = ec.inputCfg.format;
    c->out_format = ec.outputCfg.format;
    c->in_access = ec.inputCfg.accessMode;
    c->out_access = ec.outputCfg.accessMode;
    /* Snapshot BEFORE loading: do not consume a disable racing the load. */
    clarifae_config_changed(&c->config_watch);
    clarifae_config_load(&c->cfg);
    int rc = backend_open(c);
    c->configured = rc == 0;
    c->processed_frames = 0;
    return rc;
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
        if (c->backend && c->bstate) c->backend->reset(c->bstate);
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
        if (c->backend && c->bstate) c->backend->reset(c->bstate);
        return 0;
    case EFFECT_CMD_ENABLE:
    case EFFECT_CMD_DISABLE:
        if (cmdSize || reply_status(replySize, pReply, 0)) return -EINVAL;
        c->host_enabled = cmd == EFFECT_CMD_ENABLE;
        if (!c->host_enabled && c->backend && c->bstate) {
            c->backend->reset(c->bstate);
            c->bypassed = 1;
            c->processed_frames = 0;
        }
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
    case EFFECT_CMD_DUMP:
        if (!pCmd || cmdSize != sizeof(int)) return -EINVAL;
        dprintf(*(int*)pCmd, "Clarifae %s backend=%s host_enabled=%d configured=%d "
                "rate=%u channels=%d processed_frames=%llu strength=%d\n",
                c->kind ? "output" : "input", c->backend ? c->backend->name : "none",
                c->host_enabled, c->configured, c->sample_rate, c->channels,
                (unsigned long long)c->processed_frames,
                c->kind ? c->cfg.out_strength : c->cfg.strength);
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
    c->cur_mode = c->cfg.mode;
    uint32_t mask = kind == CLF_KIND_INPUT ? AUDIO_CHANNEL_IN_MONO : AUDIO_CHANNEL_OUT_STEREO;
    c->audio_config.inputCfg.samplingRate = c->audio_config.outputCfg.samplingRate = 48000;
    c->audio_config.inputCfg.channels = c->audio_config.outputCfg.channels = mask;
    c->audio_config.inputCfg.format = c->audio_config.outputCfg.format = AUDIO_FORMAT_PCM_16_BIT;
    c->audio_config.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
    c->audio_config.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
    c->audio_config.inputCfg.mask = c->audio_config.outputCfg.mask = EFFECT_CONFIG_ALL;

    *pHandle = (effect_handle_t)c;
    CLARIFAE_LOGI("effect created: %s (session %d)",
                  kind == CLF_KIND_OUTPUT ? "output" : "input", sessionId);
    return 0;
}

static int32_t lib_release(effect_handle_t handle) {
    clarifae_ctx_t* c = (clarifae_ctx_t*)handle;
    if (!c) return -EINVAL;
    backend_close(c);
    free(c->fbuf);
    free(c);
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
