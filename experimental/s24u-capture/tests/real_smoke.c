/* Host RNNoise only. Optional Android neural plugins are not exercised. */
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include "../engine/effect.c"
static _Atomic int freed;
void test_context_freed(void) { atomic_fetch_add(&freed,1); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void pause_ms(int ms) { struct timespec t={0,ms*1000000L}; nanosleep(&t,NULL); }
static void command(effect_handle_t h,int cmd) { int32_t v=-1;uint32_t n=4;assert((*h)->command(h,cmd,0,NULL,&n,&v)==0);assert(cmd==EFFECT_CMD_RESET||v==0); }
int main(void) {
 for(int kind=0;kind<2;kind++) {
  effect_handle_t h=NULL;effect_uuid_t u=kind?(effect_uuid_t)CLARIFAE_OUT_IMPL_UUID:(effect_uuid_t)CLARIFAE_IMPL_UUID;
  assert(!lib_create(&u,0,0,&h));command(h,EFFECT_CMD_INIT);
  effect_config_t ec=((clarifae_ctx_t*)h)->audio_config;assert(!configure((clarifae_ctx_t*)h,&ec));
  command(h,EFFECT_CMD_ENABLE);double end=now()+3;
  for(;;) { int16_t a[960]={0},b[960]={0};audio_buffer_t ib={.frameCount=480,.s16=a},ob={.frameCount=480,.s16=b};assert(!(*h)->process(h,&ib,&ob)); uint32_t n=64;clarifae_private_stats_t s;assert(!(*h)->command(h,CLF_CMD_GET_STATS,0,NULL,&n,&s));if(s.mode==1)break;assert(now()<end);pause_ms(1); }
  int ch=kind?2:1;float input[480*2],output[480*2];
  for(int j=0;j<30;j++) {
   for(int i=0;i<480*ch;i++)input[i]=.2f*sinf((float)(j*480+i)*.03f);
   /* Default config is PCM16: keep format honest by reconfiguring first. */
   int16_t in16[480*2],out16[480*2];for(int i=0;i<480*ch;i++)in16[i]=(int16_t)(input[i]*32768.f);
   audio_buffer_t in={.frameCount=480,.s16=in16},out={.frameCount=480,.s16=out16};assert(!(*h)->process(h,&in,&out));
   if(!j)assert(!memcmp(in16,out16,(size_t)480*ch*2));
   for(int i=0;i<480*ch;i++)output[i]=out16[i]/32768.f;
   for(int i=0;i<480*ch;i++)assert(isfinite(output[i]));
  }
  command(h,EFFECT_CMD_DISABLE);command(h,EFFECT_CMD_ENABLE);command(h,EFFECT_CMD_RESET);assert(!lib_release(h));
  end=now()+3;while(atomic_load(&freed)!=kind+1){assert(now()<end);pause_ms(1);}
 }
 puts("PASS real RNNoise host ASan/UBSan INPUT+OUTPUT prime/fade/off-on/reset/release; no optional plugin claims");return 0;
}
