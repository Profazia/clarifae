/* Test-only declarations matching the public API 26 property subset. */
#ifndef CLF_HOST_PROPERTIES_H
#define CLF_HOST_PROPERTIES_H
#include <stdint.h>
#define PROP_VALUE_MAX 92
typedef struct prop_info prop_info;
int __system_property_get(const char* name, char* value);
const prop_info* __system_property_find(const char* name);
void __system_property_read_callback(const prop_info* pi,
                                     void (*callback)(void*, const char*, const char*, uint32_t),
                                     void* cookie);
#endif
