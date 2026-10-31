#ifndef CLARIFAE_DF_BACKEND_DIAGNOSTICS_H
#define CLARIFAE_DF_BACKEND_DIAGNOSTICS_H
#include <stdint.h>
/* Backend-state pointer, not CdfState. Safe concurrent counter read only while
 * state lifetime is protected by the caller. Successful 480-sample model hops
 * per independent channel since creation/reset, not UI mode selections. */
uint64_t clarifae_deepfilternet_model_frames(const void *backend_state, int channel);
#endif
