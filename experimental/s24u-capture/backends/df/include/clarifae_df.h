/* Clarifae checked wrapper ABI v1. NOT upstream DeepFilterNet's unsafe capi.
 * Build/link only with this directory's libdf. Single caller per state;
 * independent states may run concurrently. Pointers must reference live,
 * correctly sized/aligned memory. No API can validate arbitrary C pointers.
 * create/free/reset are CONTROL-WORKER operations (allocate/deallocate).
 * process uses normalized [-1,1] mono, exactly 480 input AND output samples.
 * The upstream tract interpreter allocates per hop; NOT hard-realtime safe.
 * No logger is registered and log calls are compile-time disabled.
 * Panics are caught before returning to C (OOM/abort/UB are not catchable).
 */
#ifndef CLARIFAE_CHECKED_DF_H
#define CLARIFAE_CHECKED_DF_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CdfState CdfState;
CdfState *clarifae_df_create(const char *model_path, float atten_db, char *err, size_t err_cap);
size_t clarifae_df_frame_length(const CdfState *);
size_t clarifae_df_sample_rate(const CdfState *);
/* Algorithmic delay: 1440 samples (30 ms), except 0dB shortcut=0 samples.
 * Does not include callback FIFO priming. Backend never requests 0dB. */
size_t clarifae_df_delay_samples(const CdfState *);
/* Return 0, -EINVAL (-22), -EIO (-5; poisoned), -EFAULT (-14; caught panic).
 * No aliasing between input/output/lsnr. Finite normalized inputs required.
 * Invalid input/length leaves state intact. Internal failure poisons it;
 * output is zeroed and reset/recreate on the control worker is required.
 */
int clarifae_df_process(CdfState *, const float *, size_t, float *, size_t, float *lsnr);
int clarifae_df_set_atten_lim(CdfState *, float atten_db); /* finite [0,100], 0=bypass */
int clarifae_df_reset(CdfState *);
void clarifae_df_free(CdfState *); /* NULL allowed; not a duplicate free */
uint64_t clarifae_df_processed_frames(const CdfState *);
const char *clarifae_df_backend_name(void);
/* Metrics build only; UINT64_MAX in production. */
uint64_t clarifae_df_allocation_count(void);
#ifdef __cplusplus
}
#endif
#endif
