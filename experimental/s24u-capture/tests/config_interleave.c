/* Regression: a disable arriving after enabled was read must not be swallowed. */
#define __system_property_get sim_original_get
#define main property_baseline_main
#include "property_change.c"
#undef main
#undef __system_property_get
#include "audio_effect.h"
#include "clarifae_uuid.h"
#include "clarifae_stats.h"
static const char* armed_key;
int __system_property_get(const char*name,char*out){int n=sim_original_get(name,out);if(armed_key&&!strcmp(name,"persist.clarifae.out_targets")){const char*key=armed_key;armed_key=NULL;sim_set(key,"0",77);}return n;}
extern audio_effect_library_t AELI;
static int status(effect_handle_t h,unsigned command,void*data,unsigned bytes){int32_t result=-1;uint32_t size=4;CHECK((*h)->command(h,command,bytes,data,&size,&result)==0);CHECK(size==4);return result;}
int main(void){
 const char*keys[]={"persist.clarifae.enabled","persist.clarifae.in_enabled"};
 for(int k=0;k<2;k++){
  sim_set("persist.clarifae.enabled","1",77);sim_set("persist.clarifae.in_enabled","1",77);sim_set("persist.clarifae.mode","1",77);sim_set("persist.clarifae.strength","67",77);
  effect_uuid_t id=CLARIFAE_IMPL_UUID;effect_handle_t effect=NULL;CHECK(AELI.create_effect(&id,1,1,&effect)==0);CHECK(status(effect,EFFECT_CMD_INIT,NULL,0)==0);
  effect_config_t cfg={0};cfg.inputCfg.samplingRate=cfg.outputCfg.samplingRate=48000;cfg.inputCfg.channels=cfg.outputCfg.channels=0x10;cfg.inputCfg.format=cfg.outputCfg.format=1;cfg.inputCfg.accessMode=1;cfg.outputCfg.accessMode=0;cfg.inputCfg.mask=cfg.outputCfg.mask=EFFECT_CONFIG_ALL;
  armed_key=keys[k];CHECK(status(effect,EFFECT_CMD_SET_CONFIG,&cfg,sizeof(cfg))==0);CHECK(!armed_key);CHECK(status(effect,EFFECT_CMD_ENABLE,NULL,0)==0);
  int16_t input[480],output[480];for(int i=0;i<480;i++)input[i]=(int16_t)(i+100);
  audio_buffer_t in={.frameCount=480,.s16=input},out={.frameCount=480,.s16=output};
  for(int n=0;n<3;n++){memset(output,0,sizeof(output));CHECK((*effect)->process(effect,&in,&out)==0);CHECK(!memcmp(input,output,sizeof(input)));}
  clarifae_private_stats_t stats;uint32_t bytes=sizeof(stats);CHECK((*effect)->command(effect,CLF_CMD_GET_STATS,0,NULL,&bytes,&stats)==0);CHECK(stats.processed_frames==0);CHECK(!(stats.enabled&&stats.in_enabled));CHECK(AELI.release_effect(effect)==0);
 }
 printf("PASS config construction disable interleavings keys=enabled,in_enabled checks=%u\n",checks);return 0;
}
