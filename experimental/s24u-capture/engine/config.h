/*
 * config.h — live configuration sourced from persist.clarifae.* system
 * properties. Properties are the SELinux-safe channel audioserver can read;
 * clarifae-ctl keeps them in sync with /data/adb/clarifae/config.conf.
 */
#ifndef CLARIFAE_CONFIG_H
#define CLARIFAE_CONFIG_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct clarifae_config {
    int   enabled;       /* persist.clarifae.enabled     0|1  master */
    int   in_enabled;    /* persist.clarifae.in_enabled  0|1  clean microphone */
    int   out_enabled;   /* persist.clarifae.out_enabled 0|1  clean playback */
    int   mode;          /* persist.clarifae.mode        1|2|3 */
    int   strength;      /* persist.clarifae.strength    0..100 input wet/dry */
    int   out_strength;  /* persist.clarifae.out_strength 0..100 output wet/dry */
    int   atten_db;      /* persist.clarifae.atten_db    0..60  max suppression */
    int   vad;           /* persist.clarifae.vad         0..100 */
    int   highpass;      /* persist.clarifae.highpass    0|1  */
    int   autogain;      /* persist.clarifae.autogain    0|1  */
    int   loglevel;      /* persist.clarifae.loglevel    0..3 */
    char  targets[64];     /* persist.clarifae.targets     csv capture sources */
    char  out_targets[64]; /* persist.clarifae.out_targets csv playback streams */
} clarifae_config_t;

/* Load every persist.clarifae.* property into cfg, applying contract defaults
 * for any that are unset/empty. Also updates the global clarifae_log_level. */
void clarifae_config_load(clarifae_config_t* cfg);

/* Cheap check (property serial compare) for "did any clarifae prop change since
 * the last call?". Returns non-zero when a reload is warranted. Safe to call
 * from the realtime process() path. */
#define CLARIFAE_CONFIG_KEY_COUNT 13
#define CLARIFAE_CONFIG_VALUE_MAX 92
typedef struct {const void* property;uint32_t serial;char value[CLARIFAE_CONFIG_VALUE_MAX];} clarifae_config_slot_t;
typedef struct {clarifae_config_slot_t slots[CLARIFAE_CONFIG_KEY_COUNT];} clarifae_config_watch_t;
int clarifae_config_changed(clarifae_config_watch_t* watch);

#ifdef __cplusplus
}
#endif

#endif /* CLARIFAE_CONFIG_H */
