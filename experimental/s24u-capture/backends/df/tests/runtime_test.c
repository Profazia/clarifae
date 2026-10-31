#define _POSIX_C_SOURCE 200809L
#include "clarifae_df.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#define H 480
#define BLOCKS 60
static float reference[BLOCKS][H];
static void signal_frame(float *x,int f) {
    uint32_t r=0x1234u+(uint32_t)f;
    for(int j=0;j<H;++j) { int k=f*H+j; r=r*1664525u+1013904223u;
        x[j]=0.10f*sinf((float)k*0.031f)+0.06f*sinf((float)k*0.073f)+0.025f*((float)(r>>8)/8388608.0f-1.0f); }
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
static CdfState *make(const char *p,float atten) { char e[200]; CdfState*s=clarifae_df_create(p,atten,e,sizeof(e)); if(!s)fprintf(stderr,"model error: %s\n",e); CHECK(s); return s; }
static void *thread_run(void *state) {
    CdfState *s=state; float x[H],y[H],snr;
    for(int f=0;f<BLOCKS;++f) { signal_frame(x,f); CHECK(!clarifae_df_process(s,x,H,y,H,&snr));
        for(int j=0;j<H;++j) CHECK(fabsf(y[j]-reference[f][j])<1e-5f); }
    return NULL;
}
int main(int argc,char **argv) {
    CHECK(argc==3); const char *model=argv[1], *bad=argv[2]; char err[64];
    printf("runtime=%s\n",clarifae_df_backend_name());
    CHECK(!clarifae_df_create(NULL,100,err,sizeof(err)));
    CHECK(!clarifae_df_create("/definitely/missing/df-model",100,err,sizeof(err)));
    for(int tries=0;tries<3;++tries)CHECK(!clarifae_df_create(bad,100,err,sizeof(err))); printf("failed_model=%s\n",err);
    CHECK(!clarifae_df_create("/dev/null",100,err,sizeof(err)));
    CHECK(!clarifae_df_create(model,NAN,err,sizeof(err)));
    CHECK(!clarifae_df_create(model,101,err,sizeof(err)));
    CdfState *a=make(model,100), *b=make(model,100);
    CHECK(clarifae_df_frame_length(a)==H && clarifae_df_sample_rate(a)==48000 && clarifae_df_delay_samples(a)==1440);
    float x[H]={0},y[H],yb[H],snr=0;
    CHECK(clarifae_df_process(NULL,x,H,y,H,&snr)==-22);
    CHECK(clarifae_df_process(a,x,H-1,y,H,&snr)==-22);
    CHECK(clarifae_df_process(a,x,H,x,H,&snr)==-22);
    CHECK(clarifae_df_process(a,x,H,y,H,y)==-22);
    CHECK(clarifae_df_process(a,NULL,H,y,H,&snr)==-22);
    x[0]=NAN; CHECK(clarifae_df_process(a,x,H,y,H,&snr)==-22);
    x[0]=INFINITY; CHECK(clarifae_df_process(a,x,H,y,H,&snr)==-22);
    x[0]=1.1f; CHECK(clarifae_df_process(a,x,H,y,H,&snr)==-22);
    CHECK(clarifae_df_processed_frames(a)==0);
    CHECK(clarifae_df_set_atten_lim(a,NAN)==-22 && clarifae_df_set_atten_lim(a,-1)==-22);
    double energy=0,noise=0,total=0,max_time=0; uint64_t alloc=clarifae_df_allocation_count();
    for(int f=0;f<BLOCKS;++f) {
        signal_frame(x,f); double t=now_s(); CHECK(!clarifae_df_process(a,x,H,y,H,&snr));
        double dt=now_s()-t; total+=dt; if(dt>max_time)max_time=dt;
        CHECK(!clarifae_df_process(b,x,H,yb,H,NULL)); CHECK(isfinite(snr));
        for(int j=0;j<H;++j) { CHECK(isfinite(y[j]) && fabsf(y[j]-yb[j])<1e-5f); reference[f][j]=y[j]; energy+=y[j]*y[j]; noise+=x[j]*x[j]; }
    }
    uint64_t after=clarifae_df_allocation_count();
    CHECK(energy>1e-8 && fabs(energy-noise)>1e-4);
    CHECK(clarifae_df_processed_frames(a)==BLOCKS && clarifae_df_processed_frames(b)==BLOCKS);
    printf("model_load=PASS actual_inference=PASS frames=%llu input_energy=%.9g output_energy=%.9g mean_ms=%.4f max_ms=%.4f\n",
        (unsigned long long)clarifae_df_processed_frames(a),noise,energy,total*1000/BLOCKS,max_time*1000);
    if(alloc!=UINT64_MAX)printf("rust_allocations_for_two_streams=%llu over_hops=%d\n",(unsigned long long)(after-alloc),BLOCKS*2);
    for(int pass=0;pass<3;++pass) { CHECK(!clarifae_df_reset(a)); CHECK(!clarifae_df_reset(b));
        pthread_t ta,tb; CHECK(!pthread_create(&ta,NULL,thread_run,a)); CHECK(!pthread_create(&tb,NULL,thread_run,b));
        CHECK(!pthread_join(ta,NULL)); CHECK(!pthread_join(tb,NULL)); }
    printf("multiple_contexts=PASS concurrent_streams=PASS repeat_reset_equals_fresh=PASS\n");
    CHECK(!clarifae_df_set_atten_lim(a,0.01f)); CHECK(!clarifae_df_reset(a));
    int peak_at=-1; float peak=0;
    for(int f=0;f<12;++f) { memset(x,0,sizeof(x)); if(f==0)x[32]=0.25f;
        CHECK(!clarifae_df_process(a,x,H,y,H,&snr));
        for(int j=0;j<H;++j)if(fabsf(y[j])>peak){peak=fabsf(y[j]);peak_at=f*H+j;} }
    printf("near_transparent_impulse_peak=%d amplitude=%.8f expected=1472\n",peak_at,peak);
    CHECK(peak_at==1472 && peak>0.24f && peak<0.26f);
    clarifae_df_free(a); clarifae_df_free(b); clarifae_df_free(NULL);
    CHECK(clarifae_df_reset(NULL)==-22); printf("runtime_tests=PASS\n"); return 0;
}
