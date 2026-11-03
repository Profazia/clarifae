/* Host-only deterministic fake models: does NOT verify real neural inference. */
#include <assert.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include "../engine/effect.c"
static pthread_t host;
static _Atomic int creates, destroys, live, max_live, freed, lookups, began[4], finished[4];
static _Atomic int fail_mode, fail_process, wait_mode, allow_mode;
static _Atomic int blocked[4];
static clarifae_ctx_t *chunk_context; /* host thread only */
static int chunk_calls, chunk_modes[4];
static clarifae_config_t controls;
static uint64_t config_serial=1;
static int checks;
static double max_callback;
_Atomic int clarifae_log_level=0;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); abort(); } } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static void pause_ms(int ms) { struct timespec t={ms/1000,(ms%1000)*1000000L}; nanosleep(&t,NULL); }
static void timed(double begin) { double ms=(now()-begin)*1000; if (ms>max_callback) max_callback=ms; CHECK(ms<20); }
void test_context_freed(void) { atomic_fetch_add(&freed,1); }
void clarifae_config_load(clarifae_config_t *c) { *c=controls; }
int clarifae_config_changed(clarifae_config_watch_t *w) { uint32_t s=(uint32_t)config_serial; if (w->slots[0].serial==s) return 0; w->slots[0].serial=s; return 1; }
typedef struct { int mode, channels, rate; unsigned primed; float *delay; size_t delay_count, delay_pos; } fake_t;
static void *make(int mode,int rate,int channels) {
    (void)rate;
    assert(!pthread_equal(pthread_self(),host));
    atomic_fetch_add(&began[mode],1); atomic_fetch_add(&creates,1);
    pause_ms(300); /* mandatory slow constructor */
    while (atomic_load(&wait_mode)==mode && atomic_load(&allow_mode)!=mode) {
        atomic_store(&blocked[mode],1); pause_ms(1);
    }
    atomic_fetch_add(&finished[mode],1);
    if (atomic_load(&fail_mode)==mode) return NULL;
    fake_t *s=calloc(1,sizeof(*s)); assert(s); s->mode=mode; s->channels=channels; s->rate=rate;
    s->delay_count=(size_t)rate*40u/1000u*(size_t)channels;
    s->delay=calloc(s->delay_count,sizeof(float));assert(s->delay);
    int n=atomic_fetch_add(&live,1)+1, old=atomic_load(&max_live);
    while (n>old && !atomic_compare_exchange_weak(&max_live,&old,n)) {}
    return s;
}
static void *make1(int r,int c,char *e,int n) { (void)e;(void)n;return make(1,r,c); }
static void *make2(int r,int c,char *e,int n) { (void)e;(void)n;return make(2,r,c); }
static void *make3(int r,int c,char *e,int n) { (void)e;(void)n;return make(3,r,c); }
static int infer(void *p,float *v,int frames) {
    fake_t *s=p;
    if (!pthread_equal(pthread_self(),host)) {
        for (int i=0;i<frames*s->channels;i++) assert(v[i]==0);
        s->primed+=(unsigned)frames;
    } else assert(s->primed>=(unsigned)s->rate*60u/1000u);
    if (pthread_equal(pthread_self(),host) && atomic_load(&fail_process)) return -EIO;
    if (pthread_equal(pthread_self(),host) && chunk_context) {
        CHECK(chunk_calls<4); chunk_modes[chunk_calls++]=s->mode;
        if (chunk_calls==1) {
            /* Publish a replacement between internal chunks, deterministically.
             * This deliberately coordinated callback is NOT a timing sample. */
            atomic_store(&allow_mode,3); double end=now()+3; int ready=0;
            while (!ready && now()<end) {
                pthread_mutex_lock(&chunk_context->mailbox);
                ready=chunk_context->ready!=NULL;
                pthread_mutex_unlock(&chunk_context->mailbox);
                if (!ready) pause_ms(1);
            }
            CHECK(ready);
        }
    }
    for (int i=0;i<frames*s->channels;i++) {
        float old=v[i];v[i]=s->delay[s->delay_pos];s->delay[s->delay_pos]=old*(float)s->mode/4.f;
        if (++s->delay_pos==s->delay_count) s->delay_pos=0;
    }
    return 0;
}
static void params(void *p,float a,float b,float d,int e,int f) { (void)p;(void)a;(void)b;(void)d;(void)e;(void)f; }
static void reset(void *p) { (void)p; assert(!"callback/model reset forbidden: fresh worker build required"); }
static void destroy(void *p) { assert(!pthread_equal(pthread_self(),host)); atomic_fetch_sub(&live,1); atomic_fetch_add(&destroys,1); free(((fake_t*)p)->delay); free(p); }
static const clarifae_backend_t bs[]={
 {"rnnoise",make1,infer,params,reset,destroy}, {"dtln",make2,infer,params,reset,destroy}, {"deepfilternet",make3,infer,params,reset,destroy}};
const clarifae_backend_t *clarifae_backend_rnnoise(void) { assert(!pthread_equal(pthread_self(),host)); atomic_fetch_add(&lookups,1); return &bs[0]; }
const clarifae_backend_t *clarifae_backend_for_mode(int m) { assert(!pthread_equal(pthread_self(),host)); atomic_fetch_add(&lookups,1); return &bs[m-1]; }
static void defaults(void) { memset(&controls,0,sizeof(controls)); controls.enabled=controls.in_enabled=controls.out_enabled=1; controls.mode=1; controls.strength=controls.out_strength=70; config_serial++; }
static int command(effect_handle_t h,uint32_t cmd) { int32_t reply=-9; uint32_t n=4; double t=now(); int rc=(*h)->command(h,cmd,0,NULL,&n,&reply); timed(t); return (rc || cmd==EFFECT_CMD_RESET)?rc:reply; }
static void set_mode(int mode) { controls.mode=mode; config_serial++; }
static clarifae_private_stats_t stats(effect_handle_t h) { clarifae_private_stats_t s={0}; uint32_t n=sizeof(s); double t=now(); CHECK((*h)->command(h,CLF_CMD_GET_STATS,0,NULL,&n,&s)==0); timed(t); CHECK(n==64); return s; }
static effect_handle_t create(int kind) { effect_handle_t h=NULL; const effect_uuid_t u=kind ? (effect_uuid_t)CLARIFAE_OUT_IMPL_UUID : (effect_uuid_t)CLARIFAE_IMPL_UUID; double t=now(); CHECK(lib_create(&u,1,1,&h)==0); timed(t); return h; }
static int config(effect_handle_t h,int rate,int channels,int format,int accum) {
 effect_config_t ec={0}; ec.inputCfg.samplingRate=ec.outputCfg.samplingRate=rate;
 ec.inputCfg.channels=ec.outputCfg.channels=(1u<<channels)-1;
 ec.inputCfg.format=ec.outputCfg.format=format; ec.inputCfg.accessMode=EFFECT_BUFFER_ACCESS_READ;
 ec.outputCfg.accessMode=accum?EFFECT_BUFFER_ACCESS_ACCUMULATE:EFFECT_BUFFER_ACCESS_WRITE;
 ec.inputCfg.mask=ec.outputCfg.mask=EFFECT_CONFIG_ALL;
 int32_t reply=-9;uint32_t n=4;double t=now();CHECK((*h)->command(h,EFFECT_CMD_SET_CONFIG,sizeof(ec),&ec,&n,&reply)==0);timed(t);return reply;
}
static void pcm(effect_handle_t h,int raw) {
 clarifae_ctx_t *c=(clarifae_ctx_t*)h; int n=480*c->channels;int16_t input[480*8],output[480*8];
 for(int i=0;i<n;i++){input[i]=12000+i%29;output[i]=-3;}
 audio_buffer_t ib={.frameCount=480,.s16=input},ob={.frameCount=480,.s16=output};
 double t=now();int rc=(*h)->process(h,&ib,&ob);timed(t);CHECK(rc==(c->host_enabled?0:-ENODATA));
 if(raw)CHECK(!memcmp(input,output,(size_t)n*2));
}
static void await_started(int m,int before) { double end=now()+3; while(atomic_load(&began[m])<=before && now()<end)pause_ms(1);CHECK(atomic_load(&began[m])>before); }
static void pump_one(effect_handle_t h) {
 int16_t in[8]={0},out[8]={0};audio_buffer_t ib={.frameCount=1,.s16=in},ob={.frameCount=1,.s16=out};
 double begin=now();CHECK((*h)->process(h,&ib,&ob)==0);timed(begin);
}
static void await_mode(effect_handle_t h,int mode) {
 double end=now()+4;while(now()<end){clarifae_private_stats_t s=stats(h);if(s.mode==mode&&!s.pending_mode)return;pump_one(h);pause_ms(2);}
 fprintf(stderr,"readiness timeout expected=%d actual=%d pending=%d\n",mode,stats(h).mode,stats(h).pending_mode);CHECK(!"mode readiness timeout");
}
static void await_prepared(effect_handle_t h) {
 clarifae_ctx_t *c=(clarifae_ctx_t*)h;double end=now()+4;
 while(now()<end){pthread_mutex_lock(&c->mailbox);int ready=c->ready!=NULL;pthread_mutex_unlock(&c->mailbox);if(ready)return;pause_ms(2);}
 CHECK(!"worker prepare timeout");
}
static void await_freed(int target) { double end=now()+4;while(atomic_load(&freed)<target&&now()<end)pause_ms(2);CHECK(atomic_load(&freed)==target); }
static void close_effect(effect_handle_t h) { double t=now();CHECK(lib_release(h)==0);timed(t); }
int main(void) {
 host=pthread_self();defaults();
 /* Both directions: configured while host-off must never lookup/create. */
 for(int kind=0;kind<2;kind++) {
  defaults();
  int target=atomic_load(&freed)+1;effect_handle_t h=create(kind);int before=atomic_load(&creates);
  CHECK(config(h,48000,kind?2:1,AUDIO_FORMAT_PCM_16_BIT,0)==0);pause_ms(10);CHECK(atomic_load(&creates)==before);pcm(h,1);
  CHECK(command(h,EFFECT_CMD_ENABLE)==0);await_started(1,atomic_load(&began[1])-1);
  CHECK(stats(h).mode==0&&stats(h).pending_mode);pcm(h,1);await_mode(h,1);uint64_t first_count=stats(h).processed_frames;pcm(h,1);CHECK(stats(h).processed_frames==first_count+480);
  /* Direct-buffer/same-format reconfiguration must not discard ready state. */
  int same_before=atomic_load(&creates);CHECK(config(h,48000,kind?2:1,AUDIO_FORMAT_PCM_16_BIT,0)==0);CHECK(stats(h).mode==1&&!stats(h).pending_mode);CHECK(atomic_load(&creates)==same_before);
  /* Old ready 1 keeps running during slow 2; then slow 3. */
  set_mode(2);pcm(h,0);CHECK(stats(h).mode==1&&stats(h).pending_mode);await_mode(h,2);
  set_mode(3);pcm(h,0);CHECK(stats(h).mode==2&&stats(h).pending_mode);await_mode(h,3);
  close_effect(h);await_freed(target);
 }
 /* Barrier-controlled 2->3->2->3->2: never commit stale 3. */
 defaults();set_mode(2);effect_handle_t h=create(0);CHECK(config(h,48000,1,AUDIO_FORMAT_PCM_16_BIT,0)==0);CHECK(command(h,EFFECT_CMD_ENABLE)==0);await_mode(h,2);
 int before=atomic_load(&began[3]);atomic_store(&wait_mode,3);atomic_store(&allow_mode,0);set_mode(3);pcm(h,0);await_started(3,before);
 for(int i=0;i<200;i++){set_mode(i%2?2:3);pcm(h,0);}set_mode(2);pcm(h,0);CHECK(stats(h).mode==2&&!stats(h).pending_mode);
 atomic_store(&allow_mode,3);pause_ms(320);CHECK(stats(h).mode==2&&!stats(h).pending_mode);atomic_store(&wait_mode,0);
 /* Disable while building: immediate raw, no stale publication. */
 before=atomic_load(&began[3]);set_mode(3);pcm(h,0);await_started(3,before);CHECK(command(h,EFFECT_CMD_DISABLE)==0);pcm(h,1);pause_ms(330);CHECK(stats(h).mode==0&&!stats(h).pending_mode);
 CHECK(command(h,EFFECT_CMD_ENABLE)==0);pcm(h,1);await_mode(h,3);
 /* Property off/on invalidates; live failure counts only success. */
 uint64_t count=stats(h).processed_frames;atomic_store(&fail_process,1);pcm(h,1);CHECK(stats(h).processed_frames==count);atomic_store(&fail_process,0);await_mode(h,3);
 controls.enabled=0;config_serial++;pcm(h,1);CHECK(stats(h).mode==0);controls.enabled=1;config_serial++;pcm(h,1);await_mode(h,3);
 CHECK(command(h,EFFECT_CMD_RESET)==0);pcm(h,1);CHECK(stats(h).processed_frames==0);await_mode(h,3);pcm(h,1);
 /* Format/channels change during stale build uses the new job geometry only. */
 before=atomic_load(&began[2]);set_mode(2);pcm(h,0);await_started(2,before);CHECK(config(h,16000,2,AUDIO_FORMAT_PCM_16_BIT,0)==0);pcm(h,1);await_mode(h,2);CHECK(stats(h).channels==2&&stats(h).sample_rate==16000);
 int target=atomic_load(&freed)+1;close_effect(h);await_freed(target);
 /* Close during 300ms create returns promptly; detached worker exits/frees. */
 defaults();set_mode(3);h=create(0);CHECK(config(h,48000,1,AUDIO_FORMAT_PCM_16_BIT,0)==0);before=atomic_load(&began[3]);CHECK(command(h,EFFECT_CMD_ENABLE)==0);await_started(3,before);target=atomic_load(&freed)+1;close_effect(h);await_freed(target);
 /* Actual fallback is truthful, requested model remains not-ready. */
 defaults();set_mode(3);atomic_store(&fail_mode,3);h=create(1);CHECK(config(h,48000,2,AUDIO_FORMAT_PCM_16_BIT,0)==0);CHECK(command(h,EFFECT_CMD_ENABLE)==0);
 double end=now()+3;while(stats(h).mode!=1&&now()<end){pump_one(h);pause_ms(2);}CHECK(stats(h).requested_mode==3&&stats(h).mode==1&&stats(h).pending_mode);target=atomic_load(&freed)+1;close_effect(h);await_freed(target);atomic_store(&fail_mode,0);
 /* A model published between 2048-frame internal chunks must wait until the
  * NEXT whole callback. Both input and output exercise actual active DSP. */
 for(int kind=0;kind<2;kind++) {
  defaults();h=create(kind);CHECK(!config(h,48000,1,AUDIO_FORMAT_PCM_16_BIT,0));
  CHECK(!command(h,EFFECT_CMD_ENABLE));await_mode(h,1);
  atomic_store(&blocked[3],0);atomic_store(&wait_mode,3);atomic_store(&allow_mode,0);
  set_mode(3);pcm(h,0);double until=now()+3;
  while(!atomic_load(&blocked[3])&&now()<until)pause_ms(1);CHECK(atomic_load(&blocked[3]));
  int16_t signal[4096],result[4096];for(int i=0;i<4096;i++){signal[i]=12000;result[i]=-1;}
  audio_buffer_t input={.frameCount=4096,.s16=signal},output={.frameCount=4096,.s16=result};
  uint64_t previous=stats(h).processed_frames;chunk_calls=0;chunk_context=(clarifae_ctx_t*)h;
  CHECK(!(*h)->process(h,&input,&output));chunk_context=NULL;
  CHECK(chunk_calls==2&&chunk_modes[0]==1&&chunk_modes[1]==1);
  CHECK(stats(h).mode==1&&stats(h).pending_mode);
  CHECK(stats(h).processed_frames==previous+4096);
  pump_one(h);CHECK(stats(h).mode==3&&!stats(h).pending_mode);
  atomic_store(&wait_mode,0);target=atomic_load(&freed)+1;close_effect(h);await_freed(target);
 }
 /* OUTPUT constructed with side control OFF, enabled host, then side ON. */
 defaults();set_mode(2);controls.out_enabled=0;config_serial++;h=create(1);CHECK(config(h,48000,2,AUDIO_FORMAT_PCM_16_BIT,0)==0);int off_creates=atomic_load(&creates);CHECK(command(h,EFFECT_CMD_ENABLE)==0);pause_ms(15);pcm(h,1);CHECK(atomic_load(&creates)==off_creates);CHECK(stats(h).mode==0&&!stats(h).pending_mode);
 controls.out_enabled=1;config_serial++;pcm(h,1);await_mode(h,2);uint64_t on_count=stats(h).processed_frames;pcm(h,1);CHECK(stats(h).processed_frames==on_count+480);target=atomic_load(&freed)+1;close_effect(h);await_freed(target);
 /* GET_STATS must leave a ready state unadopted and live attribution unchanged.
  * Delayed fake models output 40ms old PCM even after worker zero prime. */
 defaults();set_mode(3);h=create(0);CHECK(config(h,48000,1,AUDIO_FORMAT_PCM_16_BIT,0)==0);CHECK(command(h,EFFECT_CMD_ENABLE)==0);await_prepared(h);
 for(int i=0;i<50;i++){CHECK(stats(h).mode==0&&stats(h).pending_mode);CHECK(stats(h).processed_frames==0);}
 for(int block=0;block<20;block++) {
  int16_t constant[480],result[480];for(int i=0;i<480;i++){constant[i]=12000;result[i]=0;}
  audio_buffer_t ib={.frameCount=480,.s16=constant},ob={.frameCount=480,.s16=result};double begin=now();CHECK((*h)->process(h,&ib,&ob)==0);timed(begin);
  CHECK(stats(h).mode==3);for(int i=0;i<480;i++)CHECK(result[i]>0);
  if(block<6)CHECK(!memcmp(constant,result,sizeof(constant))); /* exactly 60ms current dry */
 }
 /* Mode-only ready publication also cannot relabel the preceding mode-3 frames. */
 set_mode(2);pcm(h,0);uint64_t previous_count=stats(h).processed_frames;await_prepared(h);
 for(int i=0;i<50;i++){CHECK(stats(h).mode==3&&stats(h).pending_mode);CHECK(stats(h).processed_frames==previous_count);}
 pump_one(h);CHECK(stats(h).mode==2&&!stats(h).pending_mode);CHECK(stats(h).processed_frames==previous_count+1);
 target=atomic_load(&freed)+1;close_effect(h);await_freed(target);
 /* Validation/unsupported commands and float bypass accumulation. */
 defaults();controls.out_enabled=0;config_serial++;h=create(1);CHECK(config(h,48000,2,AUDIO_FORMAT_PCM_FLOAT,1)==0);CHECK(command(h,EFFECT_CMD_ENABLE)==0);
 float in[4]={.1f,-.2f,.3f,-.4f},out[4]={.1f,.1f,.1f,.1f};audio_buffer_t ib={.frameCount=2,.f32=in},ob={.frameCount=2,.f32=out};CHECK((*h)->process(h,&ib,&ob)==0);CHECK(fabsf(out[0]-.2f)<1e-6);in[0]=NAN;CHECK((*h)->process(h,&ib,&ob)==0);for(int j=0;j<4;j++)CHECK(isfinite(out[j]));
 uint32_t n=0;CHECK((*h)->command(h,123456,0,NULL,&n,NULL)==-EINVAL);CHECK(config(h,7999,1,AUDIO_FORMAT_PCM_FLOAT,0)==-EINVAL);target=atomic_load(&freed)+1;close_effect(h);await_freed(target);
 CHECK(atomic_load(&live)==0);CHECK(atomic_load(&max_live)<=3);
 printf("PASS async engine checks=%d max_callback_ms=%.3f creates=%d destroys=%d contexts_freed=%d max_live=%d\n",checks,max_callback,atomic_load(&creates),atomic_load(&destroys),atomic_load(&freed),atomic_load(&max_live));
 return 0;
}
