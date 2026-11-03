#define main fixture_baseline_main
#include "async_test.c"
#undef main
#include <float.h>
int main(void){
 host=pthread_self();unsigned scenarios=0;
 for(int host_on=0;host_on<2;host_on++)for(int accum=0;accum<2;accum++)for(int alias=0;alias<2;alias++)for(int size=0;size<7;size++){
  int frames=(int[]){1,17,192,480,2048,2049,4096}[size],n=frames*2;
  defaults();controls.out_enabled=0;config_serial++;effect_handle_t h=create(1);CHECK(!config(h,48000,2,AUDIO_FORMAT_PCM_FLOAT,accum));if(host_on)CHECK(!command(h,EFFECT_CMD_ENABLE));
  float *in=malloc((size_t)(n+4)*4),*out=malloc((size_t)(n+4)*4),*copy=malloc((size_t)n*4);CHECK(in&&out&&copy);
  for(int i=0;i<n;i++){in[i]=(i%2?-1.f:1.f)*(i%4?1.25f:2.f);out[i]=.75f;copy[i]=in[i];}in[n]=out[n]=8123.5f;in[n+1]=out[n+1]=-8123.5f;
  audio_buffer_t ib={.frameCount=(size_t)frames,.f32=in},ob={.frameCount=(size_t)frames,.f32=alias?in:out};int rc=(*h)->process(h,&ib,&ob);CHECK(rc==(host_on?0:-ENODATA));
  for(int i=0;i<n;i++){float expect=copy[i]+(accum?(alias?copy[i]:.75f):0.f);CHECK(ob.f32[i]==expect);}CHECK(ob.f32[n]==8123.5f&&ob.f32[n+1]==-8123.5f);
  for(int corrupt=0;corrupt<3;corrupt++){
   for(int i=0;i<n;i++){in[i]=.25f;out[i]=.5f;}int k=n>2?n-2:0;in[k]=corrupt==0?NAN:corrupt==1?INFINITY:-INFINITY;if(accum&&!alias)out[n-1]=NAN;
   rc=(*h)->process(h,&ib,&ob);CHECK(rc==(host_on?0:-ENODATA));for(int i=0;i<n;i++)CHECK(isfinite(ob.f32[i]));CHECK(ob.f32[n]==8123.5f&&ob.f32[n+1]==-8123.5f);
  }
  if(accum){for(int i=0;i<n;i++){in[i]=FLT_MAX;out[i]=FLT_MAX;}rc=(*h)->process(h,&ib,&ob);CHECK(rc==(host_on?0:-ENODATA));for(int i=0;i<n;i++)CHECK(ob.f32[i]==FLT_MAX);}
  int target=atomic_load(&freed)+1;close_effect(h);await_freed(target);free(in);free(out);free(copy);scenarios++;
 }
 /* Capture remains strict, with no model required. */
 defaults();controls.in_enabled=0;config_serial++;effect_handle_t h=create(0);CHECK(!config(h,48000,1,AUDIO_FORMAT_PCM_FLOAT,0));CHECK(!command(h,EFFECT_CMD_ENABLE));float a[2]={1.25f,.5f},b[2]={0};audio_buffer_t ib={.frameCount=2,.f32=a},ob={.frameCount=2,.f32=b};CHECK((*h)->process(h,&ib,&ob)==-EINVAL);a[0]=NAN;CHECK((*h)->process(h,&ib,&ob)==-EINVAL);int target=atomic_load(&freed)+1;close_effect(h);await_freed(target);CHECK(!atomic_load(&live));
 printf("PASS output total-delivery matrix scenarios=%u checks=%d capture_normalization_preserved=1 no_hardware=1\n",scenarios,checks);return 0;
}
