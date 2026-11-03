/*
 * backend.h — pluggable speech-enhancement engine interface.
 *
 * effect.c is engine-agnostic: it converts AudioFlinger's PCM to mono/interleaved
 * float and hands it to a clarifae_backend_t selected by the user's "mode":
 *   mode 1 = rnnoise        (always compiled, the shipping engine)
 *   mode 2 = dtln           (optional, CLARIFAE_WITH_DTLN, falls back to rnnoise)
 *   mode 3 = deepfilternet  (optional, CLARIFAE_WITH_DF,   falls back to rnnoise)
 */
#ifndef CLARIFAE_BACKEND_H
#define CLARIFAE_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct clarifae_backend {
    const char* name;

    /* Create engine state for a stream. Returns NULL on failure and writes a
     * short reason into err (errlen bytes) so the dispatcher can fall back. */
    void* (*create)(int sample_rate, int channels, char* err, int errlen);

    /* Enhance `frames` frames in place. Buffer is `frames * channels` floats,
     * interleaved, in the range [-32768, 32767]. Returns 0 on success. */
    int (*process)(void* st, float* inout, int frames);

    /* Live parameter update (called when persist.clarifae.* changes). */
    void (*set_params)(void* st, float strength, float atten_db, float vad,
                       int highpass, int autogain);

    /* Flush internal buffers (stream discontinuity). */
    void (*reset)(void* st);

    /* Tear down engine state. */
    void (*destroy)(void* st);
} clarifae_backend_t;

/* Always present. */
const clarifae_backend_t* clarifae_backend_rnnoise(void);

/* Optional engines — weak so the symbol resolves to NULL when not linked. */
const clarifae_backend_t* clarifae_backend_dtln(void) __attribute__((weak));
const clarifae_backend_t* clarifae_backend_deepfilternet(void) __attribute__((weak));

/* Pick a backend for a mode, with graceful fallback to rnnoise. */
const clarifae_backend_t* clarifae_backend_for_mode(int mode);

#ifdef __cplusplus
}
#endif

#endif /* CLARIFAE_BACKEND_H */
