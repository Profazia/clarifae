#ifndef CLARIFAE_PRIVATE_STATS_H
#define CLARIFAE_PRIVATE_STATS_H
#include <stdint.h>
#define CLF_CMD_GET_STATS 0x10000u
#define CLF_STATS_VERSION 2u
typedef struct {
 uint32_t size, version;
 uint64_t processed_frames;
 uint32_t sample_rate, channels;
 int32_t kind, host_enabled, configured, enabled, in_enabled, out_enabled, strength, mode;
 int32_t requested_mode, pending_mode;
} clarifae_private_stats_t;
_Static_assert(sizeof(clarifae_private_stats_t)==64,"private stats ABI");
#endif
