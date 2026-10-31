#include "audio_effect.h"
#include "clarifae_uuid.h"
#include "clarifae_stats.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef struct {effect_interface_t*itfe;effect_config_t config;int enabled;}ctx;
static int32_t process(effect_handle_t h,audio_buffer_t*in,audio_buffer_t*out){ctx*c=(ctx*)h;(void)in;const char*stage=getenv("CLF_FAULT_STAGE");if(stage&&(!strcmp(stage,"nan")||!strcmp(stage,"range"))){float*p=out->raw;for(size_t i=0;i<out->frameCount;i++)p[i]=!strcmp(stage,"nan")?NAN:1.5f;return 0;}memset(out->raw,0xee,out->frameCount*(c->config.outputCfg.format==5?4:2));return -EIO;}
static int32_t command(effect_handle_t h,uint32_t cmd,uint32_t bytes,void*data,uint32_t*size,void*reply){ctx*c=(ctx*)h;(void)bytes;const char*stage=getenv("CLF_FAULT_STAGE");
 if(cmd==CLF_CMD_GET_STATS){if(stage&&!strcmp(stage,"stats"))return -EINVAL;if(!size||!reply||*size<sizeof(clarifae_private_stats_t))return -EINVAL;clarifae_private_stats_t s={.size=sizeof(s),.version=CLF_STATS_VERSION,.sample_rate=48000,.channels=1,.host_enabled=1,.configured=1,.enabled=1,.in_enabled=1,.strength=67,.mode=1,.requested_mode=1};memcpy(reply,&s,sizeof(s));*size=sizeof(s);return 0;}
 if(cmd==EFFECT_CMD_RESET)return 0;
 if(!size||!reply||*size<4)return -EINVAL;
 *(int*)reply=0;*size=4;
 if(stage&&((cmd==EFFECT_CMD_INIT&&!strcmp(stage,"init"))||(cmd==EFFECT_CMD_SET_CONFIG&&!strcmp(stage,"config"))||(cmd==EFFECT_CMD_ENABLE&&!strcmp(stage,"enable")))){*(int*)reply=-ENOMEM;return 0;}
 if(cmd==EFFECT_CMD_SET_CONFIG)c->config=*(effect_config_t*)data;return 0;
}
static int32_t descriptor(effect_handle_t h,effect_descriptor_t*d){(void)h;memset(d,0,sizeof(*d));effect_uuid_t id=CLARIFAE_IMPL_UUID;d->uuid=id;d->apiVersion=0x20000;return 0;}
static effect_interface_t itfe={.process=process,.command=command,.get_descriptor=descriptor};
static int32_t create(const effect_uuid_t*u,int32_t session,int32_t io,effect_handle_t*h){(void)u;(void)session;(void)io;ctx*c=calloc(1,sizeof(*c));if(!c)return -ENOMEM;c->itfe=&itfe;*h=(effect_handle_t)c;return 0;}
static int32_t release(effect_handle_t h){free(h);return 0;}
static int32_t get(const effect_uuid_t*u,effect_descriptor_t*d){(void)u;return descriptor(NULL,d);}
__attribute__((visibility("default"))) audio_effect_library_t AELI={.tag=0x41454c54,.version=0x30000,.name="fault fixture",.create_effect=create,.release_effect=release,.get_descriptor=get};
