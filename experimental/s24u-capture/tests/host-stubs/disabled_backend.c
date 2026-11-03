/* Disabled-only interleaving fixture: ANY backend lookup is a test failure. */
#include "backend.h"
#include <stdlib.h>
const clarifae_backend_t* clarifae_backend_rnnoise(void) { abort(); }
const clarifae_backend_t* clarifae_backend_for_mode(int mode) { (void)mode; abort(); }
