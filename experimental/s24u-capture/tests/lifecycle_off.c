/* Fake-backend / host-API fixture only: no plugin, model, HAL or device. */
#define main fixture_baseline_main
#include "async_test.c"
#undef main
int main(void) {
 host=pthread_self(); unsigned scenarios=0;
 for(int kind=0;kind<2;kind++)for(int gate=0;gate<3;gate++)for(int mode=2;mode<=3;mode++) {
  defaults(); controls.mode=mode;
  controls.enabled=gate==1;
  if(gate!=0) { if(kind)controls.out_enabled=0;else controls.in_enabled=0; }
  config_serial++; int before_lookup=atomic_load(&lookups),before_create=atomic_load(&creates);
  int target=atomic_load(&freed)+1; effect_handle_t h=create(kind);
  CHECK(!config(h,48000,kind?2:1,AUDIO_FORMAT_PCM_16_BIT,0));
  CHECK(!command(h,EFFECT_CMD_ENABLE)); pcm(h,1);
  CHECK(!command(h,EFFECT_CMD_RESET)); pcm(h,1);
  set_mode(mode==2?3:2); pcm(h,1);
  CHECK(!config(h,16000,kind?2:1,AUDIO_FORMAT_PCM_16_BIT,0)); pcm(h,1);
  CHECK(!command(h,EFFECT_CMD_DISABLE)); pcm(h,1);
  CHECK(!command(h,EFFECT_CMD_ENABLE)); pcm(h,1); pause_ms(20);
  CHECK(stats(h).mode==0&&!stats(h).pending_mode);
  CHECK(atomic_load(&lookups)==before_lookup);CHECK(atomic_load(&creates)==before_create);
  close_effect(h);await_freed(target);scenarios++;
 }
 CHECK(!atomic_load(&live));
 printf("PASS disabled lifecycle fakebackend scenarios=%u checks=%d backend_lookups=%d creates=%d contexts_freed=%d no_hardware=1\n",scenarios,checks,atomic_load(&lookups),atomic_load(&creates),atomic_load(&freed));
 return 0;
}
