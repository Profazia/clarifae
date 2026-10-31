/*
 * config.c — read persist.clarifae.* system properties into clarifae_config_t,
 * with cheap change detection for the realtime path.
 */
#include "config.h"
#include "log.h"

#include <sys/system_properties.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Single definition of the runtime log level used by log.h everywhere. */
_Atomic int clarifae_log_level = 1;

static const char* const kProps[] = {
    "persist.clarifae.enabled",
    "persist.clarifae.in_enabled",
    "persist.clarifae.out_enabled",
    "persist.clarifae.mode",
    "persist.clarifae.strength",
    "persist.clarifae.out_strength",
    "persist.clarifae.atten_db",
    "persist.clarifae.vad",
    "persist.clarifae.highpass",
    "persist.clarifae.autogain",
    "persist.clarifae.loglevel",
    "persist.clarifae.targets",
    "persist.clarifae.out_targets",
};
#define NPROPS (sizeof(kProps) / sizeof(kProps[0]))

/* Existing legacy controls remain a fallback; vendor-audio properties are
 * readable by the vendor HAL without changing SELinux policy. */
static const char* resolved_property(const char* legacy, char vendor[96]) {
    snprintf(vendor, 96, "persist.vendor.audio.clarifae.%s", legacy + 17);
    return __system_property_find(vendor) ? vendor : legacy;
}

static int get_str(const char* name, char* out, int outlen, const char* def) {
    char buf[PROP_VALUE_MAX];
    char vendor[96];
    int n = __system_property_get(resolved_property(name, vendor), buf);
    const char* src = (n > 0) ? buf : def;
    strncpy(out, src, (size_t)outlen - 1);
    out[outlen - 1] = '\0';
    return n > 0;
}

static int get_int(const char* name, int def, int lo, int hi) {
    char buf[PROP_VALUE_MAX];
    char vendor[96];
    if (__system_property_get(resolved_property(name, vendor), buf) <= 0) return def;
    char* end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf) return def;       /* not a number -> default */
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}

void clarifae_config_load(clarifae_config_t* cfg) {
    cfg->enabled      = get_int("persist.clarifae.enabled",      1, 0, 1);
    cfg->in_enabled   = get_int("persist.clarifae.in_enabled",   1, 0, 1);
    cfg->out_enabled  = get_int("persist.clarifae.out_enabled",  1, 0, 1);
    cfg->mode         = get_int("persist.clarifae.mode",         1, 1, 3);
    cfg->strength     = get_int("persist.clarifae.strength",     70, 0, 100);
    cfg->out_strength = get_int("persist.clarifae.out_strength", 60, 0, 100);
    cfg->atten_db     = get_int("persist.clarifae.atten_db",     24, 0, 60);
    cfg->vad          = get_int("persist.clarifae.vad",          50, 0, 100);
    cfg->highpass     = get_int("persist.clarifae.highpass",     1, 0, 1);
    cfg->autogain     = get_int("persist.clarifae.autogain",     0, 0, 1);
    cfg->loglevel     = get_int("persist.clarifae.loglevel",     1, 0, 3);
    get_str("persist.clarifae.targets", cfg->targets, (int)sizeof(cfg->targets),
            "mic,voice_comm");
    get_str("persist.clarifae.out_targets", cfg->out_targets, (int)sizeof(cfg->out_targets),
            "music,voice_call");
    clarifae_log_level = cfg->loglevel;
}

/* Exact per-instance snapshots include resolved property identity and value.
 * Serial numbers alone can collide when a vendor alias takes over a legacy key. */
_Static_assert(NPROPS==CLARIFAE_CONFIG_KEY_COUNT,"config key count");
_Static_assert(PROP_VALUE_MAX==CLARIFAE_CONFIG_VALUE_MAX,"property value ABI");
static void read_snapshot(void* cookie,const char* name,const char* value,uint32_t serial) {
    (void)name;clarifae_config_slot_t* slot=cookie;slot->serial=serial;
    snprintf(slot->value,sizeof(slot->value),"%s",value);
}
int clarifae_config_changed(clarifae_config_watch_t* watch) {
    int changed=0;
    for(size_t i=0;i<NPROPS;i++) {
        char vendor[96];
        const prop_info* pi=__system_property_find(resolved_property(kProps[i],vendor));
        clarifae_config_slot_t observed={.property=pi};
        if(pi)__system_property_read_callback(pi,read_snapshot,&observed);
        clarifae_config_slot_t* previous=&watch->slots[i];
        if(previous->property!=observed.property || previous->serial!=observed.serial || strcmp(previous->value,observed.value)) {
            *previous=observed;changed=1;
        }
    }
    return changed;
}
