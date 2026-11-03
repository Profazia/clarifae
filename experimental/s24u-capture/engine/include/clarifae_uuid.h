/*
 * clarifae_uuid.h — Clarifae effect UUIDs (kept in sync with the
 * audio_effects.xml entry written by common/functions.sh).
 */
#ifndef CLARIFAE_UUID_H
#define CLARIFAE_UUID_H

#include "audio_effect.h"

/*
 * Standard AOSP noise-suppression PRE-PROCESSING type UUID (FX_IID_NS).
 *   58b4b260-8e06-11e0-aa8e-0002a5d5c51b
 * Declaring our effect with this *type* lets AudioFlinger treat Clarifae as a
 * noise-suppressor in the capture pre-processing chain.
 */
#define CLARIFAE_TYPE_NS_UUID \
    { 0x58b4b260, 0x8e06, 0x11e0, 0xaa8e, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } }
#define CLARIFAE_TYPE_NS_UUID_STR "58b4b260-8e06-11e0-aa8e-0002a5d5c51b"

/*
 * Clarifae unique implementation UUID (stable, ours):
 *   c1a71fae-0001-4a5e-9b10-43578768aaf3
 * (the node bytes spell the brand colours 43 57 87 / 68 aa f3)
 */
#define CLARIFAE_IMPL_UUID \
    { 0xc1a71fae, 0x0001, 0x4a5e, 0x9b10, { 0x43, 0x57, 0x87, 0x68, 0xaa, 0xf3 } }
#define CLARIFAE_IMPL_UUID_STR "c1a71fae-0001-4a5e-9b10-43578768aaf3"

/*
 * Clarifae OUTPUT (playback) POST-PROCESSING effect. There is no standard AOSP
 * "type" for an output denoiser, so we use a custom Clarifae type plus a second
 * implementation UUID. AudioFlinger attaches this one via <postprocess> with
 * EFFECT_FLAG_TYPE_POST_PROC. Same .so, same DSP — different descriptor.
 *   type c1a71fae-00c0-4a5e-9b10-43578768aaf3
 *   uuid c1a71fae-0002-4a5e-9b10-43578768aaf3
 */
#define CLARIFAE_OUT_TYPE_UUID \
    { 0xc1a71fae, 0x00c0, 0x4a5e, 0x9b10, { 0x43, 0x57, 0x87, 0x68, 0xaa, 0xf3 } }
#define CLARIFAE_OUT_TYPE_UUID_STR "c1a71fae-00c0-4a5e-9b10-43578768aaf3"

#define CLARIFAE_OUT_IMPL_UUID \
    { 0xc1a71fae, 0x0002, 0x4a5e, 0x9b10, { 0x43, 0x57, 0x87, 0x68, 0xaa, 0xf3 } }
#define CLARIFAE_OUT_IMPL_UUID_STR "c1a71fae-0002-4a5e-9b10-43578768aaf3"

#define CLARIFAE_LIB_NAME        "clarifae"
#define CLARIFAE_EFFECT_NAME     "clarifae_ns"   /* capture pre-processing */
#define CLARIFAE_OUT_EFFECT_NAME "clarifae_out"  /* playback post-processing */

#endif /* CLARIFAE_UUID_H */
