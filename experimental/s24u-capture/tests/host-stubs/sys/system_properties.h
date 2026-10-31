#ifndef CLF_HOST_PROPERTIES_H
#define CLF_HOST_PROPERTIES_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PROP_VALUE_MAX 92
typedef struct { int dummy; } prop_info;
static inline int __system_property_get(const char* key,char* out) {
 const char* v="";
 if(strstr(key,"out_strength"))v="14";
 else if(strstr(key,"strength"))v="67";
 else if(strstr(key,"atten_db"))v="52";
 else if(strstr(key,"vad"))v="50";
 else if(strstr(key,"autogain"))v="0";
 else if(strstr(key,"targets"))v="mic,voice_comm";
 else v="1";
 strcpy(out,v); return strlen(v);
}
static inline const prop_info* __system_property_find(const char* key) { (void)key;static const prop_info p={0};return &p; }
static inline void __system_property_read_callback(const prop_info* p,void(*cb)(void*,const char*,const char*,uint32_t),void* cookie) { (void)p; cb(cookie,"","",1); }
#endif
