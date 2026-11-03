#include "backend.h"
#include "clarifae_df.h"
#include "clarifae_df_backend_diagnostics.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static const clarifae_backend_t *be;
static void fill(float *p,int n,int ch,int rate) {
    uint32_t rng=99;
    for(int i=0;i<n;++i)for(int c=0;c<ch;++c) {rng=rng*1664525u+1013904223u;
        p[i*ch+c]=3500*sinf((float)i*6.2831853f*(c?330:220)/(float)rate)+400*((float)(rng>>8)/8388608.0f-1.0f);}
}
static void run(void *s,float *v,int n,int ch,int irregular) {
    const int blocks[]={1,7,33,479,3,127,901,2048,17,511,4097}; int k=0;
    for(int at=0;at<n;) { int b=irregular?blocks[k++%11]:n; if(b>n-at)b=n-at;
        CHECK(!be->process(s,v+(size_t)at*ch,b));at+=b; }
}
static void test(int rate,int ch) {
    char err[200]; int n=rate/3+512; size_t bytes=(size_t)n*ch*sizeof(float);
    float *in=malloc(bytes),*wet=malloc(bytes),*partition=malloc(bytes),*dry=malloc(bytes),*mix=malloc(bytes);CHECK(in&&wet&&partition&&dry&&mix);
    fill(in,n,ch,rate);memcpy(wet,in,bytes);memcpy(partition,in,bytes);memcpy(dry,in,bytes);memcpy(mix,in,bytes);
    void *a=be->create(rate,ch,err,sizeof(err));if(!a)fprintf(stderr,"create failed: %s\n",err);CHECK(a);
    run(a,wet,n,ch,0);
    uint64_t hops=clarifae_deepfilternet_model_frames(a,0);CHECK(hops>0);
    if(ch==2)CHECK(clarifae_deepfilternet_model_frames(a,1)==hops);
    be->reset(a);CHECK(clarifae_deepfilternet_model_frames(a,0)==0);run(a,partition,n,ch,1);
    if(rate==48000 && ch==2) for(int ci=0;ci<2;++ci) {
        float *one=malloc((size_t)n*sizeof(float));CHECK(one);
        for(int i=0;i<n;++i)one[i]=in[i*ch+ci];
        void *mono=be->create(rate,1,err,sizeof(err));CHECK(mono);run(mono,one,n,1,1);
        for(int i=0;i<n;++i)CHECK(fabsf(one[i]-wet[i*ch+ci])<1e-4f);
        be->destroy(mono);free(one);
    }
    double maxdiff=0,change=0;
    int prime=(rate+99)/100+(rate==48000?0:2),delay=prime+rate*3/100;
    for(int i=0;i<n*ch;++i) { double d=fabs(wet[i]-partition[i]);if(d>maxdiff)maxdiff=d;CHECK(d<1e-4);CHECK(isfinite(wet[i])&&wet[i]>=-32768&&wet[i]<=32767);change+=fabs(wet[i]-in[i]); }
    for(int i=0;i<prime*ch;++i)CHECK(wet[i]==0);
    CHECK(change>1);
    be->reset(a);be->set_params(a,0,100,0,0,0);run(a,dry,n,ch,1);
    for(int i=0;i<n;++i)for(int c=0;c<ch;++c)CHECK(dry[i*ch+c]==(i<delay?0:in[(i-delay)*ch+c]));
    be->reset(a);be->set_params(a,0.5f,100,0,0,0);run(a,mix,n,ch,1);
    for(int i=0;i<n*ch;++i)CHECK(fabsf(mix[i]-(0.5f*dry[i]+0.5f*wet[i]))<0.002f);
    float bad[8]={0};bad[3]=NAN;CHECK(be->process(a,bad,4)==-EINVAL);for(int i=0;i<4*ch;++i)CHECK(bad[i]==0);
    CHECK(be->process(a,NULL,0)==0&&be->process(a,NULL,1)==-EINVAL&&be->process(a,bad,-1)==-EINVAL&&be->process(a,bad,65537)==-EINVAL);
    be->set_params(a,NAN,NAN,0,0,0);be->reset(a);
    float extreme[960];for(int i=0;i<480*ch;++i)extreme[i]=(i&1)?FLT_MAX:-FLT_MAX;
    CHECK(!be->process(a,extreme,480));
    for(int i=0;i<480*ch;++i)CHECK(isfinite(extreme[i])&&extreme[i]>=-32768&&extreme[i]<=32767);
    be->destroy(a);
    printf("backend rate=%d ch=%d model_hops_per_channel=%llu partition_maxdiff=%.9g dry_delay=%d wet_dry_mix=PASS finite_errors=PASS\n",rate,ch,(unsigned long long)hops,maxdiff,delay);
    free(in);free(wet);free(partition);free(dry);free(mix);
}
int main(void) {
    be=clarifae_backend_deepfilternet();CHECK(be&&strcmp(be->name,"deepfilternet")==0);
    printf("backend=%s runtime=%s\n",be->name,clarifae_df_backend_name());
    CHECK(!be->create(0,1,NULL,0)&&!be->create(12345,1,NULL,0)&&!be->create(48000,0,NULL,0)&&!be->create(48000,3,NULL,0));
    const int rates[]={8000,16000,24000,32000,44100,48000,88200,96000};
    for(int i=0;i<8;++i)for(int ch=1;ch<=2;++ch)test(rates[i],ch);
    puts("backend_tests=PASS");return 0;
}
