/* Deterministic isolated benchmark controls. NEVER deploy this engine. */
#include "config.h"
#include <stdatomic.h>
#include <string.h>
_Atomic int clarifae_log_level=0;
__attribute__((visibility("default"))) const int clarifae_isolated_test_build=1;
void clarifae_config_load(clarifae_config_t*c){memset(c,0,sizeof(*c));c->enabled=c->in_enabled=c->out_enabled=1;c->mode=1;c->strength=67;c->out_strength=14;c->atten_db=52;c->vad=50;c->highpass=1;strcpy(c->targets,"mic,voice_comm");strcpy(c->out_targets,"music,voice_call");}
int clarifae_config_changed(clarifae_config_watch_t*w){(void)w;return 0;}
