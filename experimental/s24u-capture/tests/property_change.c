#include "config.h"
#include <sys/system_properties.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct prop_info{char name[96],value[92];uint32_t serial;int present;};
static struct prop_info props[40];static unsigned count,checks;
#define CHECK(x)do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
const prop_info* __system_property_find(const char*name){for(unsigned i=0;i<count;i++)if(props[i].present&&!strcmp(props[i].name,name))return &props[i];return NULL;}
int __system_property_get(const char*name,char*out){const prop_info*p=__system_property_find(name);if(!p){out[0]=0;return 0;}strcpy(out,p->value);return (int)strlen(out);}
void __system_property_read_callback(const prop_info*p,void(*cb)(void*,const char*,const char*,uint32_t),void*cookie){cb(cookie,p->name,p->value,p->serial);}
void sim_set(const char*name,const char*value,uint32_t serial){struct prop_info*p=(struct prop_info*)__system_property_find(name);if(!p){CHECK(count<40);p=&props[count++];snprintf(p->name,sizeof(p->name),"%s",name);}snprintf(p->value,sizeof(p->value),"%s",value);p->serial=serial;p->present=1;}
void sim_delete(const char*name){struct prop_info*p=(struct prop_info*)__system_property_find(name);if(p)p->present=0;}
int main(void){clarifae_config_watch_t a={0},b={0};clarifae_config_t cfg;
 sim_set("persist.clarifae.enabled","1",42);sim_set("persist.clarifae.strength","67",9);
 CHECK(clarifae_config_changed(&a));CHECK(clarifae_config_changed(&b));clarifae_config_load(&cfg);CHECK(cfg.enabled==1&&cfg.strength==67);CHECK(!clarifae_config_changed(&a));
 sim_set("persist.vendor.audio.clarifae.enabled","0",42);CHECK(clarifae_config_changed(&a));CHECK(clarifae_config_changed(&b));clarifae_config_load(&cfg);CHECK(cfg.enabled==0);CHECK(!clarifae_config_changed(&a));
 sim_set("persist.vendor.audio.clarifae.enabled","1",42);CHECK(clarifae_config_changed(&a));clarifae_config_load(&cfg);CHECK(cfg.enabled==1);
 sim_set("persist.vendor.audio.clarifae.strength","60",9);CHECK(clarifae_config_changed(&a));CHECK(clarifae_config_changed(&b));clarifae_config_load(&cfg);CHECK(cfg.strength==60);CHECK(!clarifae_config_changed(&a));CHECK(!clarifae_config_changed(&b));
 sim_delete("persist.vendor.audio.clarifae.enabled");CHECK(clarifae_config_changed(&a));clarifae_config_load(&cfg);CHECK(cfg.enabled==1);
 sim_set("persist.vendor.audio.clarifae.in_enabled","0",7);CHECK(clarifae_config_changed(&a));clarifae_config_load(&cfg);CHECK(cfg.in_enabled==0);
 printf("PASS property namespace/value takeover and independent cursors checks=%u\n",checks);return 0;}
