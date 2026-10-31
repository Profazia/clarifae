#define _GNU_SOURCE
#include "audio_hal_abi.h"
#include "clarifae_stats.h"
#include "clarifae_uuid.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>

#define MAX_DEVICES 4
#define MAX_STREAMS 16
#define MAX_BLOCK_BYTES (128u*1024u)
#define STOCK_PATH "/vendor/lib64/soundfx/libclarifae_primary_stock.so"
#define ENGINE_PATH "/vendor/lib64/soundfx/libclarifae.so"
#define SWITCH_PROP "persist.vendor.audio.clarifae.adapter"
#define TARGETS_PROP "persist.vendor.audio.clarifae.targets"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,"ClarifaeCapture",__VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,"ClarifaeCapture",__VA_ARGS__)
extern hw_module_t HMI;

typedef struct device_ctx device_ctx;
typedef struct stream_ctx stream_ctx;
struct device_ctx {
 struct audio_hw_device* dev;
 __typeof__(((struct audio_hw_device*)0)->open_input_stream) open_input;
 __typeof__(((struct audio_hw_device*)0)->close_input_stream) close_input;
 __typeof__(((struct audio_hw_device*)0)->dump) dump;
 int (*close)(hw_device_t*);
 unsigned refs, leases; bool closing;
};
struct stream_ctx {
 struct audio_stream_in* stream;
 device_ctx* owner;
 __typeof__(((struct audio_stream_in*)0)->read) read;
 __typeof__(((struct audio_stream*)0)->standby) standby;
 __typeof__(((struct audio_stream*)0)->dump) dump;
 __typeof__(((struct audio_stream*)0)->set_parameters) set_parameters;
 pthread_mutex_t io;
 unsigned refs; _Atomic bool closing;
 uint32_t rate, mask, format, channels, flags;
 int32_t handle;
 _Atomic int source;
 effect_handle_t effect;
 unsigned char* backup;
 _Atomic bool reset_pending;
 bool failed;
 double energy_in_acc, energy_out_acc;
 _Atomic uint64_t reads, bytes, dsp_frames, bypass_frames, errors;
 _Atomic uint64_t energy_in_bits, energy_out_bits;
 _Atomic int strength, mode, configured, enabled, bypass_reason;
 _Atomic int requested_mode, pending_mode, attempted_mode;
 _Atomic uint64_t reset_generation, applied_reset_generation, retry_after_ns;
 _Atomic uint64_t dsp_calls,dsp_ns,max_dsp_ns,deadline_overruns;
 unsigned slow_calls;
};
static pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t device_lifecycle=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t released=PTHREAD_COND_INITIALIZER;
static pthread_cond_t control_changed=PTHREAD_COND_INITIALIZER;
static pthread_once_t control_once=PTHREAD_ONCE_INIT;
static _Atomic int control_ready;
static _Atomic uint64_t mode_reconfigurations;
static _Atomic uint64_t frames_by_mode[4];
static device_ctx* devices[MAX_DEVICES];
static stream_ctx* streams[MAX_STREAMS];
static pthread_once_t stock_once=PTHREAD_ONCE_INIT,engine_once=PTHREAD_ONCE_INIT;
static void* stock_dso;
static void* engine_dso;
static hw_module_t* stock_module;
static audio_effect_library_t* engine;
static _Atomic uint64_t total_opens, total_closed, total_reads, total_bytes, total_dsp_frames, total_errors, skipped_opens;
static char last_stream[512]="none";
#ifdef CLF_BRIDGE_TESTING
static _Atomic int test_enabled=0;
static _Atomic int test_mode=1;
__attribute__((visibility("default"))) void clf_bridge_test_set_mode(int mode){atomic_store(&test_mode,mode);}
static char test_targets[96]="mic,voice_comm";
__attribute__((visibility("default"))) void clf_bridge_test_set_enabled(int enabled) {atomic_store(&test_enabled,enabled!=0);}
__attribute__((visibility("default"))) void clf_bridge_test_set_targets(const char* targets) {
 pthread_mutex_lock(&registry); snprintf(test_targets,sizeof(test_targets),"%s",targets?targets:""); pthread_mutex_unlock(&registry);
}
#endif

static bool writable(const void* p,size_t size) {
 uintptr_t start=(uintptr_t)p;
#if defined(__ANDROID__) && defined(__aarch64__)
 /* Strip TBI/MTE tag only for /proc/maps comparison, never from receivers. */
 start &= UINT64_C(0x00ffffffffffffff);
#endif
 if(!p || size>UINTPTR_MAX-start)return false;
 FILE* file=fopen("/proc/self/maps","r");
#ifdef __APPLE__
 /* Host test fixtures own their writable objects; Android production never uses this branch. */
 if(!file)return true;
#endif
 if(!file)return false;
 char line[512],permissions[8];unsigned long lo,hi;bool ok=false;
 while(fgets(line,sizeof(line),file)) {
  if(sscanf(line,"%lx-%lx %7s",&lo,&hi,permissions)==3 && start>=lo && start+size<=hi && permissions[1]=='w'){ok=true;break;}
 }
 fclose(file);return ok;
}
#ifdef CLF_BRIDGE_TESTING
__attribute__((visibility("default"))) int clf_bridge_test_writable(const void*p,size_t size){return writable(p,size);}
#endif
static const char* stock_path(void) {
#ifdef CLF_BRIDGE_TESTING
 const char* path=getenv("CLF_TEST_STOCK");if(path&&*path)return path;
#endif
 return STOCK_PATH;
}
static const char* engine_path(void) {
#ifdef CLF_BRIDGE_TESTING
 const char* path=getenv("CLF_TEST_ENGINE");if(path&&*path)return path;
#endif
 return ENGINE_PATH;
}
static int adapter_enabled(void) {
#ifdef CLF_BRIDGE_TESTING
 return atomic_load(&test_enabled);
#else
 char value[PROP_VALUE_MAX];return __system_property_get(SWITCH_PROP,value)>0 && !strcmp(value,"1");
#endif
}
static bool eligible_source(int source) {
 return source==0 || source==1 || source==5 || source==6 || source==7 || source==10;
}
static bool source_selected(int source) {
 char value[PROP_VALUE_MAX];
#ifdef CLF_BRIDGE_TESTING
 pthread_mutex_lock(&registry);snprintf(value,sizeof(value),"%s",test_targets);pthread_mutex_unlock(&registry);
#else
 if(__system_property_get(TARGETS_PROP,value)<=0)return false;
#endif
 const char* name=NULL;
 switch(source){case 0:case 1:name="mic";break;case 5:name="camcorder";break;case 6:name="voice_recognition";break;case 7:name="voice_comm";break;case 10:name="voice_performance";break;default:return false;}
 char* save=NULL;
 for(char* tok=strtok_r(value,",",&save);tok;tok=strtok_r(NULL,",",&save)) {
  if(!strcmp(tok,name) || (source==7&&!strcmp(tok,"voice_communication")))return true;
 }
 return false;
}
static void load_stock(void) {
 stock_dso=dlopen(stock_path(),RTLD_NOW|RTLD_LOCAL);
 if(!stock_dso){LOGW("stock load failed: %s",dlerror());return;}
 hw_module_t* candidate=(hw_module_t*)dlsym(stock_dso,"HMI");
 if(!candidate || candidate==&HMI || candidate->tag!=0x48574d54u || !candidate->id || strcmp(candidate->id,"audio") || !candidate->methods || !candidate->methods->open){LOGW("invalid or recursive stock HMI");return;}
 /* libhardware normally fills dso; the manually loaded alias needs the same field. */
 if(writable(candidate,sizeof(*candidate)))candidate->dso=stock_dso;
 Dl_info self_info={0}, module_info={0}, open_info={0};
 if(!dladdr(&HMI,&self_info) || !dladdr(candidate,&module_info) || !dladdr((void*)candidate->methods->open,&open_info) || module_info.dli_fbase==self_info.dli_fbase || open_info.dli_fbase!=module_info.dli_fbase){LOGW("stock module binding mismatch; refusing recursion");return;}
 stock_module=candidate;
}
static void load_engine(void) {
 engine_dso=dlopen(engine_path(),RTLD_NOW|RTLD_LOCAL);
 if(!engine_dso){LOGW("engine unavailable; stock capture remains usable: %s",dlerror());return;}
#ifndef CLF_BRIDGE_TESTING
 if(dlsym(engine_dso,"clarifae_isolated_test_build")){LOGW("test-only engine refused in production; passthrough");return;}
#endif
 audio_effect_library_t* candidate=dlsym(engine_dso,"AELI");
 if(!candidate || candidate->tag!=0x41454c54u || candidate->version!=0x30000u || !candidate->create_effect || !candidate->release_effect || !candidate->get_descriptor){LOGW("engine ABI rejected; passthrough");return;}
 effect_uuid_t id=CLARIFAE_IMPL_UUID;effect_descriptor_t d;
 if(candidate->get_descriptor(&id,&d) || d.apiVersion!=0x20000u || memcmp(&id,&d.uuid,sizeof(id))){LOGW("engine descriptor rejected; passthrough");return;}
 engine=candidate;
}
static device_ctx* pin_device(const struct audio_hw_device* dev) {
 pthread_mutex_lock(&registry);device_ctx* result=NULL;
 for(int i=0;i<MAX_DEVICES;i++)if(devices[i]&&devices[i]->dev==dev&&!devices[i]->closing){result=devices[i];result->refs++;break;}
 pthread_mutex_unlock(&registry);return result;
}
static void unpin_device(device_ctx* d){pthread_mutex_lock(&registry);if(--d->refs==0)pthread_cond_broadcast(&released);pthread_mutex_unlock(&registry);}
static stream_ctx* pin_stream(const struct audio_stream_in* stream) {
 pthread_mutex_lock(&registry);stream_ctx* result=NULL;
 for(int i=0;i<MAX_STREAMS;i++)if(streams[i]&&streams[i]->stream==stream&&!streams[i]->closing){result=streams[i];result->refs++;break;}
 pthread_mutex_unlock(&registry);return result;
}
static void unpin_stream(stream_ctx* s){pthread_mutex_lock(&registry);if(--s->refs==0)pthread_cond_broadcast(&released);pthread_mutex_unlock(&registry);}
static int effect_status(effect_handle_t effect,uint32_t cmd,void* data,uint32_t bytes) {
 int32_t status=-EINVAL;uint32_t size=sizeof(status);
 int rc=(*effect)->command(effect,cmd,bytes,data,&size,&status);
 return rc?rc:(size==sizeof(status)?status:-EINVAL);
}
static int desired_mode(void) {
#ifdef CLF_BRIDGE_TESTING
 return atomic_load(&test_mode);
#else
 char value[PROP_VALUE_MAX];if(__system_property_get("persist.vendor.audio.clarifae.mode",value)<=0)return 0;
 char*end=NULL;long mode=strtol(value,&end,10);return end!=value&&!*end&&mode>=1&&mode<=3?(int)mode:0;
#endif
}
static int read_effect_stats(effect_handle_t effect,clarifae_private_stats_t* stats) {
 uint32_t size=sizeof(*stats);memset(stats,0,sizeof(*stats));
 if(!effect || (*effect)->command(effect,CLF_CMD_GET_STATS,0,NULL,&size,stats) || size!=sizeof(*stats) || stats->size!=sizeof(*stats) || stats->version!=CLF_STATS_VERSION)return -EINVAL;
 return 0;
}
static void cache_stats(stream_ctx*s,const clarifae_private_stats_t*stats) {
 atomic_store(&s->strength,stats->strength);atomic_store(&s->mode,stats->mode);
 atomic_store(&s->configured,stats->configured);atomic_store(&s->enabled,stats->enabled&&stats->in_enabled&&stats->host_enabled);
 atomic_store(&s->requested_mode,stats->requested_mode);atomic_store(&s->pending_mode,stats->pending_mode);
}
static int effect_stats(stream_ctx*s,clarifae_private_stats_t*stats) {
 int rc=read_effect_stats(s->effect,stats);if(!rc)cache_stats(s,stats);return rc;
}
static effect_handle_t build_effect(stream_ctx*s,clarifae_private_stats_t*stats) {
 pthread_once(&engine_once,load_engine);if(!engine)return NULL;
 effect_handle_t effect=NULL;effect_uuid_t id=CLARIFAE_IMPL_UUID;
 if(engine->create_effect(&id,s->handle,s->handle,&effect) || !effect || !*effect)return NULL;
 effect_config_t cfg={0};
 cfg.inputCfg.samplingRate=cfg.outputCfg.samplingRate=s->rate;
 cfg.inputCfg.channels=cfg.outputCfg.channels=s->mask;
 cfg.inputCfg.format=cfg.outputCfg.format=(uint8_t)s->format;
 cfg.inputCfg.accessMode=EFFECT_BUFFER_ACCESS_READ;cfg.outputCfg.accessMode=EFFECT_BUFFER_ACCESS_WRITE;
 cfg.inputCfg.mask=cfg.outputCfg.mask=EFFECT_CONFIG_ALL;
 if(effect_status(effect,EFFECT_CMD_INIT,NULL,0) || effect_status(effect,EFFECT_CMD_SET_CONFIG,&cfg,sizeof(cfg)) || effect_status(effect,EFFECT_CMD_ENABLE,NULL,0) || read_effect_stats(effect,stats)) {
  engine->release_effect(effect);LOGW("capture engine initialization failed; passthrough");return NULL;
 }
 return effect;
}
static void prepare_engine(stream_ctx*s) {
 clarifae_private_stats_t stats;s->effect=build_effect(s,&stats);
 int requested=desired_mode();
 if(s->effect){cache_stats(s,&stats);requested=stats.requested_mode;}
 atomic_store(&s->attempted_mode,requested);
}
static uint64_t monotonic_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
static void* control_worker(void*unused) {
 (void)unused;
#ifdef __ANDROID__
 pthread_setname_np(pthread_self(),"ClfCaptureCtl");
#endif
 for(;;) {
  struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=200000000;
  if(until.tv_nsec>=1000000000){until.tv_sec++;until.tv_nsec-=1000000000;}
  pthread_mutex_lock(&registry);pthread_cond_timedwait(&control_changed,&registry,&until);pthread_mutex_unlock(&registry);
  int wanted=desired_mode();if(wanted<1||wanted>3||!adapter_enabled())continue;
  stream_ctx* jobs[MAX_STREAMS];int count=0;uint64_t now=monotonic_ns();
  pthread_mutex_lock(&registry);
  for(int i=0;i<MAX_STREAMS;i++)if(streams[i]&&!streams[i]->closing&&now>=atomic_load(&streams[i]->retry_after_ns)&&(atomic_load(&streams[i]->attempted_mode)!=wanted || atomic_load(&streams[i]->reset_generation)!=atomic_load(&streams[i]->applied_reset_generation))){jobs[count]=streams[i];jobs[count++]->refs++;}
  pthread_mutex_unlock(&registry);
  for(int i=0;i<count;i++) {
   stream_ctx*current=jobs[i];
   uint64_t reset_epoch=atomic_load(&current->reset_generation);
   clarifae_private_stats_t stats;effect_handle_t next=build_effect(current,&stats);
   if(!next){atomic_store(&current->attempted_mode,-1);atomic_store(&current->retry_after_ns,monotonic_ns()+2000000000ull);unpin_stream(current);continue;}
   pthread_mutex_lock(&current->io);
   pthread_mutex_lock(&registry);bool closing=current->closing;pthread_mutex_unlock(&registry);
   effect_handle_t old=NULL;
   if(!closing && desired_mode()==wanted && stats.requested_mode==wanted && atomic_load(&current->reset_generation)==reset_epoch) {
    old=current->effect;current->effect=next;next=NULL;current->failed=false;current->reset_pending=false;
    cache_stats(current,&stats);atomic_store(&current->attempted_mode,wanted);atomic_store(&current->retry_after_ns,0);atomic_store(&current->applied_reset_generation,reset_epoch);current->slow_calls=0;atomic_fetch_add(&mode_reconfigurations,1);
    LOGI("mode activated io=%d requested=%d active=%d strength=%d off_thread=1",current->handle,wanted,stats.mode,stats.strength);
   } else {atomic_store(&current->attempted_mode,-1);atomic_store(&current->retry_after_ns,0);}
   pthread_mutex_unlock(&current->io);
   /* Interpreter/model construction and destruction never run in read(). */
   if(old)engine->release_effect(old);if(next)engine->release_effect(next);
   unpin_stream(current);
  }
 }
 return NULL;
}
static void start_control_worker(void) {
 pthread_t thread;int rc=pthread_create(&thread,NULL,control_worker,NULL);
 if(!rc){pthread_detach(thread);atomic_store(&control_ready,1);}else LOGW("mode worker unavailable (%d); mode applies on capture reopen",rc);
}
static void reset_engine(stream_ctx* s) {
 if(s->effect)(*s->effect)->command(s->effect,EFFECT_CMD_RESET,0,NULL,NULL,NULL);
}
static uint64_t double_bits(double value){uint64_t bits;memcpy(&bits,&value,sizeof(bits));return bits;}
static double bits_double(uint64_t bits){double value;memcpy(&value,&bits,sizeof(value));return value;}
static double energy(const void* buffer,size_t bytes,uint32_t format) {
 double sum=0;
 if(format==AUDIO_FORMAT_PCM_FLOAT){const float*p=buffer;for(size_t i=0;i<bytes/sizeof(float);i++)sum+=(double)p[i]*p[i];}
 else{const int16_t*p=buffer;for(size_t i=0;i<bytes/sizeof(int16_t);i++){double v=p[i]/32768.0;sum+=v*v;}}
 return sum;
}
static const char* backend_name(int mode){return mode==1?"rnnoise":(mode==2?"dtln":(mode==3?"deepfilternet":"none"));}
static const char* bypass_name(int reason) {
 static const char* const names[]={"processing","adapter_off","source_excluded","format_changed","mic_muted","engine_unavailable","invalid_block","dsp_failed","nonfinite_float","mute_query_failed","stock_read_error","effect_disabled","closing","reset_preparing"};
 return reason>=0 && (size_t)reason<sizeof(names)/sizeof(names[0])?names[reason]:"unknown";
}
static void describe_stream(stream_ctx* s,char* out,size_t size) {
 double ei=bits_double(atomic_load_explicit(&s->energy_in_bits,memory_order_relaxed));
 double eo=bits_double(atomic_load_explicit(&s->energy_out_bits,memory_order_relaxed));
 double db=ei>0&&eo>0?10*log10(ei/eo):0;
 snprintf(out,size,"io=%d source=%d rate=%u channels=%u format=%u reads=%"PRIu64" bytes=%"PRIu64" processed_frames=%"PRIu64" bypass_frames=%"PRIu64" errors=%"PRIu64" dsp_mean_ms=%.3f dsp_max_ms=%.3f deadline_overruns=%"PRIu64" strength=%d mode=%d backend=%s requested_mode=%d pending_mode=%d configured=%d enabled=%d bypass_reason=%s energy_reduction_db=%.3f",
  s->handle,atomic_load(&s->source),s->rate,s->channels,s->format,atomic_load(&s->reads),atomic_load(&s->bytes),atomic_load(&s->dsp_frames),atomic_load(&s->bypass_frames),atomic_load(&s->errors),atomic_load(&s->dsp_calls)?atomic_load(&s->dsp_ns)/(1e6*atomic_load(&s->dsp_calls)):0.0,atomic_load(&s->max_dsp_ns)/1e6,atomic_load(&s->deadline_overruns),atomic_load(&s->strength),atomic_load(&s->mode),backend_name(atomic_load(&s->mode)),atomic_load(&s->requested_mode),atomic_load(&s->pending_mode),atomic_load(&s->configured),atomic_load(&s->enabled),bypass_name(atomic_load(&s->bypass_reason)),db);
}
static ssize_t bridge_read(struct audio_stream_in* stream,void* buffer,size_t bytes) {
 stream_ctx* s=pin_stream(stream);if(!s)return -ENODEV;
 pthread_mutex_lock(&s->io);
 ssize_t got=s->read(stream,buffer,bytes);
 atomic_fetch_add(&s->reads,1);atomic_fetch_add(&total_reads,1);
 if(got<=0){s->reset_pending=true;atomic_store(&s->bypass_reason,10);if(got<0){atomic_fetch_add(&s->errors,1);atomic_fetch_add(&total_errors,1);}goto done;}
 atomic_fetch_add(&s->bytes,(uint64_t)got);atomic_fetch_add(&total_bytes,(uint64_t)got);
 size_t frame_bytes=(s->format==AUDIO_FORMAT_PCM_FLOAT?4u:2u)*s->channels;
 int reason=0;
 if(!buffer || (size_t)got>bytes || (size_t)got>MAX_BLOCK_BYTES || (size_t)got%frame_bytes)reason=6;
 else if(atomic_load(&s->closing))reason=12;
 else if(s->failed)reason=7;
 else if(!s->effect)reason=5;
 else if(!adapter_enabled())reason=1;
 else if(!source_selected(atomic_load(&s->source)))reason=2;
 /* Never reallocate/reconfigure in read. Dynamic changes pass through until reopen. */
 else if(stream->common.get_sample_rate(&stream->common)!=s->rate || stream->common.get_channels(&stream->common)!=s->mask || stream->common.get_format(&stream->common)!=s->format)reason=3;
 else {
  bool muted=false;
  if(!s->owner->dev->get_mic_mute || s->owner->dev->get_mic_mute(s->owner->dev,&muted))reason=9;
  else if(muted)reason=4;
 }
 bool ready=reason==0;
 if(ready && s->format==AUDIO_FORMAT_PCM_FLOAT){const float*p=buffer;for(size_t i=0;i<(size_t)got/4;i++)if(!isfinite(p[i])||fabsf(p[i])>1.0f){ready=false;reason=8;break;}}
 atomic_store(&s->bypass_reason,reason);
 if(!ready){if(reason==4 && buffer && (size_t)got<=bytes)memset(buffer,0,(size_t)got);s->reset_pending=true;atomic_fetch_add(&s->bypass_frames,(uint64_t)got/frame_bytes);goto done;}
 if(atomic_exchange(&s->reset_pending,false)){
  if(atomic_load(&s->mode)==3){atomic_fetch_add(&s->reset_generation,1);pthread_cond_signal(&control_changed);}
  else reset_engine(s);
 }
 if(atomic_load(&s->reset_generation)!=atomic_load(&s->applied_reset_generation)){
  atomic_store(&s->bypass_reason,13);atomic_fetch_add(&s->bypass_frames,(uint64_t)got/frame_bytes);goto done;
 }
 memcpy(s->backup,buffer,(size_t)got);
 clarifae_private_stats_t before,after;
 if(effect_stats(s,&before))goto failed;
 audio_buffer_t in={.frameCount=(size_t)got/frame_bytes,.raw=buffer};
 audio_buffer_t out=in;
 struct timespec begin,end;clock_gettime(CLOCK_MONOTONIC,&begin);
 int process_result=(*s->effect)->process(s->effect,&in,&out);
 clock_gettime(CLOCK_MONOTONIC,&end);
 uint64_t elapsed=(uint64_t)(end.tv_sec-begin.tv_sec)*1000000000ull+(uint64_t)(end.tv_nsec-begin.tv_nsec);
 atomic_fetch_add(&s->dsp_calls,1);atomic_fetch_add(&s->dsp_ns,elapsed);
 uint64_t maximum=atomic_load(&s->max_dsp_ns);while(elapsed>maximum&&!atomic_compare_exchange_weak(&s->max_dsp_ns,&maximum,elapsed)){}
 uint64_t budget=in.frameCount*1000000000ull/s->rate;
 if(elapsed>budget)atomic_fetch_add(&s->deadline_overruns,1);
 if(elapsed>budget*2)s->slow_calls++;else s->slow_calls=0;
 if(process_result==-EAGAIN){memcpy(buffer,s->backup,(size_t)got);atomic_fetch_add(&s->reset_generation,1);atomic_store(&s->bypass_reason,13);atomic_fetch_add(&s->bypass_frames,in.frameCount);pthread_cond_signal(&control_changed);goto done;}
 if(process_result || s->slow_calls>=3 || effect_stats(s,&after))goto failed;
 if(s->format==AUDIO_FORMAT_PCM_FLOAT){const float*values=buffer;for(size_t i=0;i<(size_t)got/4;i++)if(!isfinite(values[i]) || fabsf(values[i])>1.0f)goto failed;}
 uint64_t processed=after.processed_frames>=before.processed_frames?after.processed_frames-before.processed_frames:after.processed_frames;
 if(processed) {
  uint64_t previous=atomic_fetch_add(&s->dsp_frames,processed);atomic_fetch_add(&total_dsp_frames,processed);
  if(after.mode>=1&&after.mode<=3)atomic_fetch_add(&frames_by_mode[after.mode],processed);
  s->energy_in_acc+=energy(s->backup,(size_t)got,s->format);s->energy_out_acc+=energy(buffer,(size_t)got,s->format);
  atomic_store(&s->energy_in_bits,double_bits(s->energy_in_acc));atomic_store(&s->energy_out_bits,double_bits(s->energy_out_acc));
  if(!previous)LOGI("processing capture io=%d source=%d rate=%u ch=%u strength=%d backend=%s",s->handle,atomic_load(&s->source),s->rate,s->channels,after.strength,backend_name(after.mode));
 } else {atomic_store(&s->bypass_reason,11);atomic_fetch_add(&s->bypass_frames,(uint64_t)got/frame_bytes);}
 goto done;
failed:
 memcpy(buffer,s->backup,(size_t)got);s->failed=true;s->reset_pending=true;atomic_store(&s->bypass_reason,7);
 atomic_fetch_add(&s->errors,1);atomic_fetch_add(&total_errors,1);
 atomic_fetch_add(&s->bypass_frames,(uint64_t)got/frame_bytes);
 LOGW("capture DSP failed io=%d; original bytes restored, passthrough until reopen",s->handle);
done:
 pthread_mutex_unlock(&s->io);unpin_stream(s);return got;
}
static int bridge_standby(struct audio_stream* stream) {
 stream_ctx*s=pin_stream((struct audio_stream_in*)stream);if(!s)return -ENODEV;
 int rc=s->standby?s->standby(stream):0;atomic_store(&s->reset_pending,true);
 unpin_stream(s);return rc;
}
static int parse_source(const char* parameters,int fallback) {
 if(!parameters || strnlen(parameters,4097)>4096)return fallback;
 const char*p=parameters;
 while(*p){const char*end=strchr(p,';');size_t len=end?(size_t)(end-p):strlen(p);
  if(len>=13&&!strncmp(p,"input_source=",13)){char*tail=NULL;long n=strtol(p+13,&tail,10);if(tail>p+13 && tail==p+len && n>=0&&n<=INT32_MAX)return (int)n;return -1;}
  if(!end)break;p=end+1;
 }
 return fallback;
}
static int bridge_set_parameters(struct audio_stream* stream,const char* parameters) {
 stream_ctx*s=pin_stream((struct audio_stream_in*)stream);if(!s)return -ENODEV;
 int rc=s->set_parameters?s->set_parameters(stream,parameters):-ENOSYS;
 if(!rc){int source=parse_source(parameters,atomic_load(&s->source));atomic_store(&s->source,source);s->reset_pending=true;}
 unpin_stream(s);return rc;
}
static void diagnostic_write(int fd,const char* bytes,size_t size) {
 /* Do not change process-wide handlers or propagate our own SIGPIPE. */
#ifdef __APPLE__
 int original_nosigpipe=fcntl(fd,F_GETNOSIGPIPE);
 if(original_nosigpipe>=0)(void)fcntl(fd,F_SETNOSIGPIPE,1);
#endif
 sigset_t one,old,pending;sigemptyset(&one);sigaddset(&one,SIGPIPE);
 bool masked=pthread_sigmask(SIG_BLOCK,&one,&old)==0;
 bool was_pending=false;if(masked&&sigpending(&pending)==0)was_pending=sigismember(&pending,SIGPIPE)==1;
 ssize_t result=write(fd,bytes,size);
 if(masked && result<0 && errno==EPIPE && !was_pending && sigpending(&pending)==0 && sigismember(&pending,SIGPIPE)==1){int signal_number;(void)sigwait(&one,&signal_number);}
 if(masked)(void)pthread_sigmask(SIG_SETMASK,&old,NULL);
#ifdef __APPLE__
 if(original_nosigpipe>=0)(void)fcntl(fd,F_SETNOSIGPIPE,original_nosigpipe);
#endif
}
static int bridge_stream_dump(const struct audio_stream* stream,int fd) {
 stream_ctx*s=pin_stream((const struct audio_stream_in*)stream);if(!s)return -ENODEV;
 int rc=s->dump?s->dump(stream,fd):0;char summary[512],output[640];describe_stream(s,summary,sizeof(summary));
 int enabled=adapter_enabled();unpin_stream(s);
 int n=snprintf(output,sizeof(output),"ClarifaeCaptureBridge %s adapter=%d\n",summary,enabled);
 if(n>0)diagnostic_write(fd,output,(size_t)n<sizeof(output)?(size_t)n:sizeof(output)-1);return rc;
}
static int bridge_device_dump(const struct audio_hw_device* dev,int fd) {
 device_ctx*d=pin_device(dev);if(!d)return -ENODEV;
 int rc=d->dump?d->dump(dev,fd):0;
 char output[11000];size_t used=0;
 int n=snprintf(output,sizeof(output),"\nClarifaeCaptureBridge version=2 adapter=%d control_worker=%d mode_reconfigurations=%"PRIu64" opened=%"PRIu64" closed=%"PRIu64" reads=%"PRIu64" bytes=%"PRIu64" processed_frames=%"PRIu64" errors=%"PRIu64" skipped_opens=%"PRIu64"\nClarifaeCaptureBridge rnnoise_frames=%"PRIu64" dtln_frames=%"PRIu64" deepfilternet_frames=%"PRIu64"\n",
  adapter_enabled(),atomic_load(&control_ready),atomic_load(&mode_reconfigurations),atomic_load(&total_opens),atomic_load(&total_closed),atomic_load(&total_reads),atomic_load(&total_bytes),atomic_load(&total_dsp_frames),atomic_load(&total_errors),atomic_load(&skipped_opens),atomic_load(&frames_by_mode[1]),atomic_load(&frames_by_mode[2]),atomic_load(&frames_by_mode[3]));
 if(n>0)used=(size_t)n<sizeof(output)?(size_t)n:sizeof(output)-1;
 stream_ctx* live[MAX_STREAMS];int count=0;char last[512];
 pthread_mutex_lock(&registry);snprintf(last,sizeof(last),"%s",last_stream);
 for(int i=0;i<MAX_STREAMS;i++)if(streams[i]&&streams[i]->owner==d&&!streams[i]->closing){live[count]=streams[i];live[count++]->refs++;}
 pthread_mutex_unlock(&registry);
 n=snprintf(output+used,sizeof(output)-used,"ClarifaeCaptureBridge last=%s\n",last);
 if(n>0)used+=(size_t)n<sizeof(output)-used?(size_t)n:sizeof(output)-used-1;
 for(int i=0;i<count;i++){
  char summary[512];describe_stream(live[i],summary,sizeof(summary));unpin_stream(live[i]);
  n=snprintf(output+used,sizeof(output)-used,"ClarifaeCaptureBridge live %s\n",summary);
  if(n>0)used+=(size_t)n<sizeof(output)-used?(size_t)n:sizeof(output)-used-1;
 }
 unpin_device(d);if(used)diagnostic_write(fd,output,used);return rc;
}
static void destroy_stream(stream_ctx*s) {
 if(s->effect&&engine)engine->release_effect(s->effect);
 free(s->backup);pthread_mutex_destroy(&s->io);free(s);
}
static int bridge_open_input(struct audio_hw_device* dev,audio_io_handle_t handle,audio_devices_t device,struct audio_config* cfg,struct audio_stream_in** result,audio_input_flags_t flags,const char* address,audio_source_t source) {
 device_ctx*d=pin_device(dev);if(!d)return -ENODEV;
 int rc=d->open_input(dev,handle,device,cfg,result,flags,address,source);
 if(rc || !result || !*result){unpin_device(d);return rc;}
 struct audio_stream_in* in=*result;
 /* RAW/hotword/MMAP/ultrasound/lookback and telephony streams stay untouched. */
 bool supported=!(flags&0x716u) && eligible_source(source) && writable(in,sizeof(*in)) && in->read && in->common.get_format && in->common.get_channels && in->common.get_sample_rate;
 uint32_t format=0,mask=0,rate=0,ch=0;
 if(supported){format=in->common.get_format(&in->common);mask=in->common.get_channels(&in->common);rate=in->common.get_sample_rate(&in->common);ch=__builtin_popcount(mask&0x3fffffffu);supported=(format==AUDIO_FORMAT_PCM_16_BIT||format==AUDIO_FORMAT_PCM_FLOAT)&&rate>=8000&&rate<=192000&&ch>=1&&ch<=2;}
 if(!supported){atomic_fetch_add(&skipped_opens,1);unpin_device(d);return rc;}
 stream_ctx*s=calloc(1,sizeof(*s));if(!s){atomic_fetch_add(&skipped_opens,1);unpin_device(d);return rc;}
 s->backup=malloc(MAX_BLOCK_BYTES);if(!s->backup){free(s);atomic_fetch_add(&skipped_opens,1);unpin_device(d);return rc;}
 pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);pthread_mutex_init(&s->io,&attr);pthread_mutexattr_destroy(&attr);
 s->stream=in;s->owner=d;s->read=in->read;s->standby=in->common.standby;s->dump=in->common.dump;s->set_parameters=in->common.set_parameters;
 s->rate=rate;s->mask=mask;s->channels=ch;s->format=format;s->flags=flags;s->handle=handle;atomic_store(&s->source,source);s->reset_pending=true;
 prepare_engine(s);if(s->effect)s->reset_pending=false;
 pthread_mutex_lock(&registry);int slot=-1;for(int i=0;i<MAX_STREAMS;i++)if(!streams[i]){slot=i;break;}
 if(slot>=0){streams[slot]=s;pthread_cond_broadcast(&control_changed);in->read=bridge_read;in->common.standby=bridge_standby;in->common.dump=bridge_stream_dump;in->common.set_parameters=bridge_set_parameters;}
 pthread_mutex_unlock(&registry);
 if(slot<0){destroy_stream(s);atomic_fetch_add(&skipped_opens,1);}else{atomic_fetch_add(&total_opens,1);LOGI("capture attached io=%d source=%d rate=%u ch=%u engine=%d adapter=%d",handle,source,rate,ch,s->effect!=NULL,adapter_enabled());}
 unpin_device(d);return rc;
}
static void bridge_close_input(struct audio_hw_device* dev,struct audio_stream_in* input) {
 device_ctx*d=pin_device(dev);if(!d)return;
 pthread_mutex_lock(&registry);stream_ctx*s=NULL;int slot=-1;
 for(int i=0;i<MAX_STREAMS;i++)if(streams[i]&&streams[i]->stream==input&&streams[i]->owner==d){s=streams[i];slot=i;break;}
 if(s&&s->closing){pthread_mutex_unlock(&registry);unpin_device(d);return;}
 if(s)s->closing=true;
 pthread_mutex_unlock(&registry);
 /* Stock standby can cancel an in-flight stock read; do not hold our locks. */
 if(s){atomic_store(&s->reset_pending,true);if(s->standby)(void)s->standby(&input->common);}
 pthread_mutex_lock(&registry);
 if(s){while(s->refs)pthread_cond_wait(&released,&registry);streams[slot]=NULL;describe_stream(s,last_stream,sizeof(last_stream));}
 pthread_mutex_unlock(&registry);
 if(s){input->read=s->read;input->common.standby=s->standby;input->common.dump=s->dump;input->common.set_parameters=s->set_parameters;}
 d->close_input(dev,input);
 if(s){destroy_stream(s);atomic_fetch_add(&total_closed,1);}
 unpin_device(d);
}
static int bridge_device_close(hw_device_t* common) {
 struct audio_hw_device* dev=(struct audio_hw_device*)common;
 pthread_mutex_lock(&device_lifecycle);pthread_mutex_lock(&registry);
 device_ctx*d=NULL;int slot=-1;
 for(int i=0;i<MAX_DEVICES;i++)if(devices[i]&&devices[i]->dev==dev){d=devices[i];slot=i;break;}
 if(!d||d->closing){pthread_mutex_unlock(&registry);pthread_mutex_unlock(&device_lifecycle);return -ENODEV;}
 if(d->leases>1) {
  pthread_mutex_unlock(&registry);
  int rc=d->close(common);
  if(!rc){pthread_mutex_lock(&registry);d->leases--;pthread_mutex_unlock(&registry);}
  pthread_mutex_unlock(&device_lifecycle);return rc;
 }
 d->closing=true;while(d->refs)pthread_cond_wait(&released,&registry);
 for(int i=0;i<MAX_STREAMS;i++)if(streams[i]&&streams[i]->owner==d){d->closing=false;pthread_mutex_unlock(&registry);pthread_mutex_unlock(&device_lifecycle);return -EBUSY;}
 pthread_mutex_unlock(&registry);
 dev->open_input_stream=d->open_input;dev->close_input_stream=d->close_input;dev->dump=d->dump;common->close=d->close;
 int rc=d->close(common);
 pthread_mutex_lock(&registry);
 if(!rc)devices[slot]=NULL;
 else {
  /* A failed stock close leaves a valid device lease: retain interception. */
  dev->open_input_stream=bridge_open_input;dev->close_input_stream=bridge_close_input;dev->dump=bridge_device_dump;common->close=bridge_device_close;d->closing=false;
 }
 pthread_mutex_unlock(&registry);if(!rc)free(d);
 pthread_mutex_unlock(&device_lifecycle);return rc;
}
static int bridge_open(const hw_module_t* module,const char* id,hw_device_t** result) {
 (void)module;pthread_once(&stock_once,load_stock);
 if(!stock_module)return -ENODEV;
 pthread_mutex_lock(&device_lifecycle);
 int rc=stock_module->methods->open(stock_module,id,result);
 if(rc||!result||!*result){pthread_mutex_unlock(&device_lifecycle);return rc;}
 struct audio_hw_device* dev=(struct audio_hw_device*)*result;
 pthread_mutex_lock(&registry);device_ctx* existing=NULL;
 for(int i=0;i<MAX_DEVICES;i++)if(devices[i]&&devices[i]->dev==dev){existing=devices[i];existing->leases++;break;}
 pthread_mutex_unlock(&registry);
 if(existing){pthread_mutex_unlock(&device_lifecycle);return rc;}
 bool memory_writable=writable(dev,sizeof(*dev));
 if(dev->common.tag!=0x48574454u || dev->common.version!=0x302 || !dev->open_input_stream || !dev->close_input_stream || !dev->common.close || !memory_writable){LOGW("stock device ABI not supported: tag=%08x version=%08x open=%d close=%d common_close=%d writable=%d address=%p; forwarding unchanged",dev->common.tag,dev->common.version,dev->open_input_stream!=NULL,dev->close_input_stream!=NULL,dev->common.close!=NULL,memory_writable,(void*)dev);pthread_mutex_unlock(&device_lifecycle);return rc;}
 device_ctx*d=calloc(1,sizeof(*d));if(!d){pthread_mutex_unlock(&device_lifecycle);return rc;}
 d->dev=dev;d->leases=1;d->open_input=dev->open_input_stream;d->close_input=dev->close_input_stream;d->dump=dev->dump;d->close=dev->common.close;
 pthread_mutex_lock(&registry);int slot=-1;for(int i=0;i<MAX_DEVICES;i++)if(!devices[i]){slot=i;break;}
 if(slot>=0){devices[slot]=d;dev->open_input_stream=bridge_open_input;dev->close_input_stream=bridge_close_input;dev->dump=bridge_device_dump;dev->common.close=bridge_device_close;}
 pthread_mutex_unlock(&registry);
 if(slot<0)free(d);else {pthread_once(&control_once,start_control_worker);LOGI("stock device opened version=%x; passthrough gate adapter=%d",dev->common.version,adapter_enabled());}
 pthread_mutex_unlock(&device_lifecycle);return rc;
}
static hw_module_methods_t bridge_methods={.open=bridge_open};
#if defined(__ELF__)
__attribute__((visibility("protected")))
#else
__attribute__((visibility("default")))
#endif
hw_module_t HMI={
 .tag=0x48574d54u,.module_api_version=1,.hal_api_version=0x100,.id="audio",
 .name="Clarifae fail-open capture adapter",.author="Clarifae repair",
 .methods=&bridge_methods,.dso=NULL,.reserved={0}
};
