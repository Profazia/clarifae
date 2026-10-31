#include "sys/system_properties.h"
#include "properties.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>

struct prop_info {
    char name[128];
    char value[PROP_VALUE_MAX];
    uint32_t serial;
};
static struct prop_info properties[32];
static size_t count;
static unsigned snapshot_reads;
unsigned host_property_snapshot_reads(void) { return snapshot_reads; }
static char hook_name[128];
static void (*get_hook)(void);

void host_properties_reset(void) {
    memset(properties, 0, sizeof(properties));
    count = 0;
    snapshot_reads = 0;
    get_hook = NULL;
    hook_name[0] = '\0';
}

const prop_info* __system_property_find(const char* name) {
    for (size_t i = 0; i < count; ++i)
        if (strcmp(name, properties[i].name) == 0)
            return &properties[i];
    return NULL;
}

void host_property_set_serial(const char* name, const char* value, uint32_t serial) {
    assert(strlen(name) < sizeof(properties[0].name));
    assert(strlen(value) < PROP_VALUE_MAX);
    struct prop_info* pi = (struct prop_info*)__system_property_find(name);
    if (!pi) {
        assert(count < sizeof(properties) / sizeof(properties[0]));
        pi = &properties[count++];
        strcpy(pi->name, name);
    }
    strcpy(pi->value, value);
    pi->serial = serial;
}

void host_property_set(const char* name, const char* value) {
    const prop_info* pi = __system_property_find(name);
    host_property_set_serial(name, value, pi ? pi->serial + 1u : 1u);
}

void host_property_after_get(const char* name, void (*hook)(void)) {
    assert(strlen(name) < sizeof(hook_name));
    strcpy(hook_name, name);
    get_hook = hook;
}

int __system_property_get(const char* name, char* value) {
    const prop_info* pi = __system_property_find(name);
    strcpy(value, pi ? pi->value : "");
    int length = (int)strlen(value);
    if (get_hook && strcmp(name, hook_name) == 0) {
        void (*hook)(void) = get_hook;
        get_hook = NULL;
        hook();
    }
    return length;
}

void __system_property_read_callback(const prop_info* pi,
                                     void (*callback)(void*, const char*, const char*, uint32_t),
                                     void* cookie) {
    /* Deterministic tests are single-threaded; emulate a consistent read. */
    ++snapshot_reads;
    char value[PROP_VALUE_MAX];
    strcpy(value, pi->value);
    callback(cookie, pi->name, value, pi->serial);
}
