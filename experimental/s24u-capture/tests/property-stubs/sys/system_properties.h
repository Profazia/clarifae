#ifndef CLF_PROPERTY_SIM_H
#define CLF_PROPERTY_SIM_H
#include <stdint.h>
#define PROP_VALUE_MAX 92
typedef struct prop_info prop_info;
int __system_property_get(const char*,char*);
const prop_info* __system_property_find(const char*);
void __system_property_read_callback(const prop_info*,void(*)(void*,const char*,const char*,uint32_t),void*);
void sim_set(const char*,const char*,uint32_t);
void sim_delete(const char*);
#endif
