#define _GNU_SOURCE
#include "fake_vendor.h"
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <fcntl.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define DECLARE(name) static __typeof__(&name) p_##name
DECLARE(fake_reset);DECLARE(fake_module);DECLARE(fake_get_state);DECLARE(fake_check_integrity);
DECLARE(fake_set_open_device_result);DECLARE(fake_set_open_input_result);DECLARE(fake_set_config);
DECLARE(fake_set_read_result);DECLARE(fake_set_next_read_result);DECLARE(fake_set_read_default);
DECLARE(fake_set_standby_result);DECLARE(fake_set_parameters_result);DECLARE(fake_set_dump_results);
DECLARE(fake_rewind_stream);DECLARE(fake_set_stream_config);
DECLARE(fake_set_singleton_mode);DECLARE(fake_block_reads);DECLARE(fake_wait_read_entered);
DECLARE(fake_set_close_device_result);DECLARE(fake_set_standby_unblocks);
static void(*set_enabled)(int);static void(*set_targets)(const char*);
static hw_module_t* mod;static int next_handle=1;
typedef struct {audio_hw_device_t*d;audio_stream_in_t*s;_Atomic int started,done;ssize_t result;} race_args;
static void* race_reader(void*p){race_args*a=p;unsigned char bytes[960];a->result=a->s->read(a->s,bytes,sizeof(bytes));atomic_store(&a->done,1);return NULL;}
typedef struct{audio_hw_device_t*d;int fd;_Atomic int started,done;int result;}dump_args;
static void* blocked_dump(void*p){dump_args*a=p;atomic_store(&a->started,1);a->result=a->d->dump(a->d,a->fd);atomic_store(&a->done,1);return NULL;}
static void* race_closer(void*p){race_args*a=p;atomic_store(&a->started,1);a->d->close_input_stream(a->d,a->s);atomic_store(&a->done,1);return NULL;}

#define LOAD(name) do{*(void**)(&p_##name)=dlsym(stock,#name);CHECK(p_##name);}while(0)
static audio_hw_device_t* open_device(void) {
 hw_device_t* common=NULL;CHECK(mod->methods->open(mod,AUDIO_HARDWARE_INTERFACE,&common)==0);CHECK(common);
 audio_hw_device_t* d=(audio_hw_device_t*)common;
 const struct fake_state* state=p_fake_get_state();CHECK(d==state->last_device);CHECK(state->last_device_open.module==p_fake_module());CHECK(p_fake_check_integrity()==0);return d;
}
static audio_stream_in_t* open_stream(audio_hw_device_t*d,int source,unsigned flags) {
 audio_stream_in_t*s=NULL;struct audio_config* cfg=(struct audio_config*)(uintptr_t)1;const char* address="isolated-fixture";
 CHECK(d->open_input_stream(d,next_handle++,0x80000004u,cfg,&s,flags,address,source)==0);CHECK(s);
 const struct fake_state*state=p_fake_get_state();CHECK(state->last_input_open.device==d);CHECK(state->last_input_open.config==cfg);CHECK(state->last_input_open.address==address);CHECK(state->last_input_open.source==source);CHECK(state->last_input_open.flags==flags);CHECK(state->last_input_open.stream_out==&s);CHECK(p_fake_check_integrity()==0);return s;
}
static void close_pair(audio_hw_device_t*d,audio_stream_in_t*s){d->close_input_stream(d,s);CHECK(p_fake_check_integrity()==0);CHECK(d->common.close(&d->common)==0);CHECK(p_fake_get_state()->live_devices==0);CHECK(p_fake_get_state()->live_streams==0);}
static char* dump_device(audio_hw_device_t*d) {
 FILE*f=tmpfile();CHECK(f);CHECK(d->dump(d,fileno(f))==0);CHECK(!fflush(f));CHECK(!fseek(f,0,SEEK_END));long size=ftell(f);CHECK(size>=0);CHECK(!fseek(f,0,SEEK_SET));char*out=calloc(1,(size_t)size+1);CHECK(out);CHECK(fread(out,1,(size_t)size,f)==(size_t)size);fclose(f);return out;
}
/* Await real inference and finish the live dry entry window before wet assertions. */
static void await_async(audio_stream_in_t*s) {
 int16_t pcm[480];
 for(int i=0;i<25;i++){CHECK(s->read(s,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));usleep(2000);}
}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void test_bypass(int source,unsigned flags,unsigned rate,unsigned mask,unsigned format) {
 p_fake_reset();p_fake_set_config(rate,mask,format);audio_hw_device_t*d=open_device();audio_stream_in_t*s=open_stream(d,source,flags);
 unsigned char a[3840+32],b[3840+32];memset(a,0xa5,sizeof(a));memset(b,0xa5,sizeof(b));set_enabled(0);
 CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);CHECK(s->read(s,a+16,3840)==3840);
 CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(1);CHECK(s->read(s,b+16,3840)==3840);CHECK(!memcmp(a,b,sizeof(a)));close_pair(d,s);
}
int main(int argc,char**argv) {
 setvbuf(stdout,NULL,_IONBF,0);alarm(30);CHECK(argc>=4);const char*scenario=argc>4?argv[4]:"normal";setenv("CLF_TEST_STOCK",argv[2],1);setenv("CLF_TEST_ENGINE",argv[3],1);
 void*stock=dlopen(argv[2],RTLD_NOW|RTLD_LOCAL);if(!stock){fprintf(stderr,"fixture: %s\n",dlerror());return 2;}
 LOAD(fake_reset);LOAD(fake_module);LOAD(fake_get_state);LOAD(fake_check_integrity);LOAD(fake_set_open_device_result);LOAD(fake_set_open_input_result);LOAD(fake_set_config);LOAD(fake_set_read_result);LOAD(fake_set_next_read_result);LOAD(fake_set_read_default);LOAD(fake_set_standby_result);LOAD(fake_set_parameters_result);LOAD(fake_set_dump_results);LOAD(fake_rewind_stream);LOAD(fake_set_stream_config);LOAD(fake_set_singleton_mode);LOAD(fake_block_reads);LOAD(fake_wait_read_entered);LOAD(fake_set_close_device_result);LOAD(fake_set_standby_unblocks);
 void*bridge=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!bridge){fprintf(stderr,"bridge: %s\n",dlerror());return 2;}
 mod=dlsym(bridge,"HMI");*(void**)(&set_enabled)=dlsym(bridge,"clf_bridge_test_set_enabled");*(void**)(&set_targets)=dlsym(bridge,"clf_bridge_test_set_targets");CHECK(mod&&set_enabled&&set_targets);
 int(*check_writable)(const void*,size_t);*(void**)(&check_writable)=dlsym(bridge,"clf_bridge_test_writable");CHECK(check_writable);void*rw=malloc(256);CHECK(rw);CHECK(check_writable(rw,256));
#ifdef __ANDROID__
 void*tagged=(void*)(((uintptr_t)rw&UINT64_C(0x00ffffffffffffff))|UINT64_C(0xb400000000000000));CHECK(check_writable(tagged,256));CHECK(!check_writable(NULL,1));
#endif
 free(rw);CHECK(mod->tag==HARDWARE_MODULE_TAG);CHECK(mod!=p_fake_module());
 p_fake_reset();p_fake_set_open_device_result(-EIO);hw_device_t*bad=NULL;CHECK(mod->methods->open(mod,AUDIO_HARDWARE_INTERFACE,&bad)==-EIO);CHECK(!bad);
 p_fake_reset();audio_hw_device_t*d=open_device();p_fake_set_open_input_result(-EAGAIN);audio_stream_in_t*s=NULL;
 CHECK(d->open_input_stream(d,4,0x80000004u,NULL,&s,0,"forwarded",7)==-EAGAIN);CHECK(!s);CHECK(d->common.close(&d->common)==0);
 /* Processing/fault fixtures begin enabled. A disabled open now deliberately
  * creates no DSP; hot enable is prepared asynchronously by the control worker. */
 set_enabled(1);p_fake_reset();if(!strcmp(scenario,"nan")||!strcmp(scenario,"range"))p_fake_set_config(48000,0x10,5);d=open_device();s=open_stream(d,1,0);
 CHECK(d->common.close(&d->common)==-EBUSY);
 if(!strcmp(scenario,"normal"))await_async(s);
 unsigned char raw[960+32],out[960+32];memset(raw,0xa5,sizeof(raw));memset(out,0xa5,sizeof(out));set_enabled(0);
 CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);CHECK(s->read(s,raw+16,960)==960);
 if(!strcmp(scenario,"normal")) {
  char*disabled=dump_device(d);CHECK(strstr(disabled,"adapter=0"));
  CHECK(strstr(disabled,"bypass_reason=adapter_off"));free(disabled);
 }
 CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(1);if(!strcmp(scenario,"normal"))await_async(s);CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);CHECK(s->read(s,out+16,960)==960);
 if(strcmp(scenario,"normal")) {
  CHECK(!memcmp(raw,out,sizeof(raw)));
  CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);memset(out,0xa5,sizeof(out));CHECK(s->read(s,out+16,960)==960);CHECK(!memcmp(raw,out,sizeof(raw)));
  char*text=dump_device(d);puts(text);free(text);close_pair(d,s);
  printf("PASS fail-open scenario=%s checks=%u\n",scenario,checks);return 0;
 }
 CHECK(memcmp(raw+16,out+16,960)!=0);CHECK(!memcmp(raw,out,16));CHECK(!memcmp(raw+976,out+976,16));
 /* Engine-off baseline and engine-on identical injected source, not a microphone. */
 CHECK(s->common.standby(&s->common)==0);CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(0);double before=0,after=0,mean=0,max=0;
 int16_t pcm[480];for(int i=0;i<300;i++){CHECK(s->read(s,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));if(i>=100)for(int j=0;j<480;j++)before+=(double)pcm[j]*pcm[j];}
 CHECK(s->common.standby(&s->common)==0);CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(1);await_async(s);
 for(int i=0;i<300;i++){double t=seconds();CHECK(s->read(s,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));t=(seconds()-t)*1e3;mean+=t;if(t>max)max=t;if(i>=100)for(int j=0;j<480;j++){after+=(double)pcm[j]*pcm[j];CHECK(pcm[j]!=32767&&pcm[j]!=-32768);}}
 double db=10*log10(before/after);CHECK(isfinite(db)&&db>3);printf("injected_noise_reduction_db=%.3f bridge_read_mean_ms=%.3f max_ms=%.3f\n",db,mean/300,max);
 char*text=dump_device(d);CHECK(strstr(text,"processed_frames=")&&strstr(text,"strength=67"));puts(text);free(text);
 effect_handle_t token=(effect_handle_t)(uintptr_t)0x1234;CHECK(s->common.add_audio_effect(&s->common,token)==0);CHECK(p_fake_get_state()->last_added_effect==token);CHECK(s->common.remove_audio_effect(&s->common,token)==0);CHECK(p_fake_get_state()->last_removed_effect==token);
 /* Original errors and non-frame-aligned/oversized responses are never enhanced. */
 const ssize_t responses[]={-EIO,0,7,961};
 for(int i=0;i<4;i++){memset(out,0xa5,sizeof(out));CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);p_fake_set_next_read_result(responses[i]);ssize_t n=s->read(s,out+16,960);CHECK(n==responses[i]);CHECK(!memcmp(raw,out,16));CHECK(!memcmp(raw+976,out+976,16));if(n<=0)CHECK(out[16]==0xa5);else CHECK(!memcmp(raw+16,out+16,n>960?960:(size_t)n));}
 p_fake_set_read_default();
 CHECK(p_fake_set_stream_config(s,48000,0x10,5)==0);CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(0);CHECK(s->read(s,raw+16,960)==960);CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(1);CHECK(s->read(s,out+16,960)==960);CHECK(!memcmp(raw,out,sizeof(raw)));
 CHECK(p_fake_set_stream_config(s,48000,0x10,1)==0);
 p_fake_set_standby_result(-EIO);CHECK(s->common.standby(&s->common)==-EIO);p_fake_set_standby_result(0);
 p_fake_set_parameters_result(-ENOSYS);CHECK(s->common.set_parameters(&s->common,"input_source=5")==-ENOSYS);p_fake_set_parameters_result(0);CHECK(s->common.set_parameters(&s->common,"input_source=5")==0);
 CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(0);CHECK(s->read(s,raw+16,960)==960);CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(1);CHECK(s->read(s,out+16,960)==960);CHECK(!memcmp(raw,out,sizeof(raw)));
 p_fake_set_dump_results(-EIO,-EINVAL);FILE*f=tmpfile();CHECK(f);CHECK(d->dump(d,fileno(f))==-EIO);CHECK(s->common.dump(&s->common,fileno(f))==-EINVAL);fclose(f);p_fake_set_dump_results(0,0);
 /* Unknown or malformed input-source metadata must stop DSP conservatively. */
 const char*unknown_sources[]={"input_source=1999","input_source=bad","input_source="};
 for(int ui=0;ui<3;ui++){CHECK(s->common.set_parameters(&s->common,unknown_sources[ui])==0);CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(0);CHECK(s->read(s,raw+16,960)==960);CHECK(p_fake_rewind_stream(s,FAKE_DEFAULT_SEED)==0);set_enabled(1);CHECK(s->read(s,out+16,960)==960);CHECK(!memcmp(raw,out,sizeof(raw)));}
 close_pair(d,s);
 p_fake_reset();d=open_device();s=open_stream(d,1,0);set_enabled(1);CHECK(s->read(s,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));CHECK(d->set_mic_mute(d,true)==0);memset(pcm,0xa5,sizeof(pcm));CHECK(s->read(s,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));for(size_t mi=0;mi<480;mi++)CHECK(pcm[mi]==0);CHECK(d->set_mic_mute(d,false)==0);CHECK(s->read(s,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));close_pair(d,s);
 test_bypass(5,0,48000,0x10,1);test_bypass(2,0,48000,0x10,1);test_bypass(3,0,48000,0x10,1);test_bypass(4,0,48000,0x10,1);test_bypass(9,0,48000,0x10,1);
 test_bypass(1,4,48000,0x10,1);test_bypass(1,16,48000,0x10,1);test_bypass(1,2,48000,0x10,1);test_bypass(1,256,48000,0x10,1);
 test_bypass(1,0,48000,0x10,3);test_bypass(1,0,4000,0x10,1);test_bypass(1,0,48000,0x3f,1);
 for(int floating=0;floating<2;floating++)for(int channels=1;channels<=2;channels++){
  p_fake_reset();p_fake_set_config(44100,channels==1?0x10:0xc,floating?5:1);d=open_device();s=open_stream(d,7,0);set_enabled(1);
  unsigned char*buf=malloc(8192);CHECK(buf);for(int j=0;j<20;j++)CHECK(s->read(s,buf,8192)==8192);free(buf);close_pair(d,s);
 }
 /* Stock primary module returns one refcounted singleton. Non-final close
  * must retain hooks, even while another lease owns a live input stream. */
 p_fake_reset();p_fake_set_singleton_mode(1);audio_hw_device_t*d1=open_device(),*d2=open_device();CHECK(d1==d2);s=open_stream(d1,1,0);
 CHECK(d1->common.close(&d1->common)==0);set_enabled(1);CHECK(s->read(s,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));
 audio_hw_device_t*d3=open_device();CHECK(d3==d2);CHECK(d2->common.close(&d2->common)==0);
 d3->close_input_stream(d3,s);p_fake_set_close_device_result(-EIO);CHECK(d3->common.close(&d3->common)==-EIO);CHECK(d3->init_check(d3)==0);p_fake_set_close_device_result(0);
 s=open_stream(d3,7,0);CHECK(s->read(s,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));close_pair(d3,s);
 /* A close waits for an already pinned stock read, without freeing its DSP. */
 p_fake_reset();d=open_device();s=open_stream(d,1,0);p_fake_block_reads(1);
 race_args reader={.d=d,.s=s},closer={.d=d,.s=s};pthread_t rt,ct;
 CHECK(!pthread_create(&rt,NULL,race_reader,&reader));p_fake_wait_read_entered();
 CHECK(!pthread_create(&ct,NULL,race_closer,&closer));while(!atomic_load(&closer.started))sched_yield();
 struct timespec delay={.tv_sec=0,.tv_nsec=20000000};nanosleep(&delay,NULL);CHECK(!atomic_load(&closer.done));
 p_fake_block_reads(0);CHECK(!pthread_join(rt,NULL));CHECK(!pthread_join(ct,NULL));CHECK(reader.result==960);CHECK(atomic_load(&closer.done));CHECK(p_fake_get_state()->live_streams==0);CHECK(d->common.close(&d->common)==0);
 /* Stock standby must be reachable to cancel a stalled original read. */
 p_fake_reset();p_fake_set_standby_unblocks(1);d=open_device();s=open_stream(d,1,0);p_fake_block_reads(1);
 race_args cancel_reader={.d=d,.s=s},cancel_close={.d=d,.s=s};
 CHECK(!pthread_create(&rt,NULL,race_reader,&cancel_reader));p_fake_wait_read_entered();
 CHECK(!pthread_create(&ct,NULL,race_closer,&cancel_close));CHECK(!pthread_join(ct,NULL));CHECK(!pthread_join(rt,NULL));CHECK(cancel_reader.result==960);CHECK(atomic_load(&cancel_close.done));CHECK(d->common.close(&d->common)==0);
 /* Our own full-pipe diagnostics must not pin a device or capture stream. */
 p_fake_reset();d=open_device();s=open_stream(d,1,0);int pipefd[2];CHECK(!pipe(pipefd));int flags=fcntl(pipefd[1],F_GETFL);CHECK(flags>=0);CHECK(!fcntl(pipefd[1],F_SETFL,flags|O_NONBLOCK));char filler[1024]={0};while(write(pipefd[1],filler,sizeof(filler))>0){}CHECK(errno==EAGAIN||errno==EWOULDBLOCK);CHECK(!fcntl(pipefd[1],F_SETFL,flags));
 dump_args blocked={.d=d,.fd=pipefd[1]};pthread_t dt;CHECK(!pthread_create(&dt,NULL,blocked_dump,&blocked));while(!atomic_load(&blocked.started))sched_yield();nanosleep(&delay,NULL);CHECK(!atomic_load(&blocked.done));
 d->close_input_stream(d,s);CHECK(d->common.close(&d->common)==0);char drain[8192];CHECK(read(pipefd[0],drain,sizeof(drain))>0);CHECK(!pthread_join(dt,NULL));CHECK(blocked.result==0);close(pipefd[0]);close(pipefd[1]);
 /* A closed consumer must not deliver SIGPIPE from our added diagnostic write. */
 p_fake_reset();d=open_device();CHECK(!pipe(pipefd));close(pipefd[0]);CHECK(d->dump(d,pipefd[1])==0);close(pipefd[1]);CHECK(d->common.close(&d->common)==0);
 CHECK(p_fake_check_integrity()==0);printf("PASS adapter forwarding/processing/bypass/lifecycle checks=%u\n",checks);return 0;
}
