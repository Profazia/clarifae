/* MOCK only: tests worker handoff, not a neural backend. Never deploy. */
#include "audio_effect.h"
#include "clarifae_uuid.h"
#include "clarifae_stats.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define API __attribute__((visibility("default")))
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int armed,entered,captured,release1,release2;
static _Atomic int wanted=1;
API void fixture_mode(int mode){atomic_store(&wanted,mode);}
API void fixture_arm(void){pthread_mutex_lock(&lock);armed=1;entered=captured=release1=release2=0;pthread_mutex_unlock(&lock);}
API void fixture_wait(int stage){pthread_mutex_lock(&lock);while(!(stage==1?entered:captured))pthread_cond_wait(&cond,&lock);pthread_mutex_unlock(&lock);}
API void fixture_release(int stage){pthread_mutex_lock(&lock);if(stage==1)release1=1;else release2=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&lock);}
typedef struct{effect_interface_t*itfe;int mode;effect_config_t cfg;uint64_t frames;}state;
static int32_t process(effect_handle_t h,audio_buffer_t*in,audio_buffer_t*out){state*s=(state*)h;size_t bytes=in->frameCount*(s->cfg.inputCfg.format==5?4:2);memmove(out->raw,in->raw,bytes);s->frames+=in->frameCount;return 0;}
static int32_t command(effect_handle_t h,uint32_t cmd,uint32_t size,void*data,uint32_t*replysize,void*reply){state*s=(state*)h;(void)size;
 if(cmd==CLF_CMD_GET_STATS){if(!replysize||!reply||*replysize<sizeof(clarifae_private_stats_t))return -EINVAL;clarifae_private_stats_t st={.size=sizeof(st),.version=CLF_STATS_VERSION,.processed_frames=s->frames,.sample_rate=48000,.channels=1,.host_enabled=1,.configured=1,.enabled=1,.in_enabled=1,.strength=67,.mode=s->mode,.requested_mode=s->mode};memcpy(reply,&st,sizeof(st));*replysize=sizeof(st);return 0;}
 if(cmd==EFFECT_CMD_RESET){s->frames=0;return 0;}
 if(!replysize||!reply||*replysize<4)return -EINVAL;*(int*)reply=0;*replysize=4;
 if(cmd==EFFECT_CMD_SET_CONFIG){s->cfg=*(effect_config_t*)data;pthread_mutex_lock(&lock);if(armed){armed=0;entered=1;pthread_cond_broadcast(&cond);while(!release1)pthread_cond_wait(&cond,&lock);s->mode=atomic_load(&wanted);captured=1;pthread_cond_broadcast(&cond);while(!release2)pthread_cond_wait(&cond,&lock);}else s->mode=atomic_load(&wanted);pthread_mutex_unlock(&lock);}
 return 0;
}
static int32_t desc(effect_handle_t h,effect_descriptor_t*d){(void)h;memset(d,0,sizeof(*d));effect_uuid_t id=CLARIFAE_IMPL_UUID;d->uuid=id;d->apiVersion=0x20000;return 0;}
static effect_interface_t iface={.process=process,.command=command,.get_descriptor=desc};
static int32_t create(const effect_uuid_t*u,int32_t session,int32_t io,effect_handle_t*out){(void)u;(void)session;(void)io;state*s=calloc(1,sizeof(*s));if(!s)return -ENOMEM;s->itfe=&iface;*out=(effect_handle_t)s;return 0;}
static int32_t release(effect_handle_t h){free(h);return 0;}
static int32_t get(const effect_uuid_t*u,effect_descriptor_t*d){(void)u;return desc(NULL,d);}
API audio_effect_library_t AELI={.tag=0x41454c54,.version=0x30000,.name="MOCK mode control fixture",.create_effect=create,.release_effect=release,.get_descriptor=get};
