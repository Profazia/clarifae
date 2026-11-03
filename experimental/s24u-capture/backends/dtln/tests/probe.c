/* Isolated non-microphone test. Never reads a device, never changes a mode.
 * Optional argv[1]=rate, argv[2]=channels, argv[3]=sample_count. */
#include "backend.h"
#include "tflite/c/c_api.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc,char **argv) {
 int rate=argc>1?atoi(argv[1]):16000, channels=argc>2?atoi(argv[2]):1;
 int frames=argc>3?atoi(argv[3]):rate;
 if(frames<1 || frames>1920000 || channels<1 || channels>8) return 2;
 const clarifae_backend_t *b=clarifae_backend_dtln();char err[256]={0};
 void *s=b->create(rate,channels,err,sizeof(err));
 if(!s){fprintf(stderr,"create: %s\n",err);return 1;}
 float *buf=calloc((size_t)frames*channels,sizeof(float));if(!buf){b->destroy(s);return 2;}
 for(int i=0;i<frames;++i)for(int c=0;c<channels;++c)
  buf[(size_t)i*channels+c]=8000*sinf(6.28318530718f*(220+110*c)*i/rate);
 clock_t start=clock();int rc=b->process(s,buf,frames);double cpu=(double)(clock()-start)/CLOCKS_PER_SEC;
 double energy=0;int finite=1;for(int i=0;i<frames*channels;++i){finite &= isfinite(buf[i]);energy+=(double)buf[i]*buf[i];}
 printf("{\"runtime\":\"%s\",\"rate\":%d,\"channels\":%d,\"frames\":%d,\"rc\":%d,\"finite\":%d,\"rms\":%.6g,\"cpu_seconds\":%.6g}\n",TfLiteVersion(),rate,channels,frames,rc,finite,sqrt(energy/(frames*channels)),cpu);
 b->reset(s);b->destroy(s);free(buf);return rc||!finite;
}
