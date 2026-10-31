#define _GNU_SOURCE
#include "audio_hal_abi.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)do{if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define FN(name,signature) signature name;*(void**)(&name)=dlsym(engine,#name);CHECK(name)
int main(int argc,char**argv){CHECK(argc==4);alarm(10);setenv("CLF_TEST_STOCK",argv[2],1);setenv("CLF_TEST_ENGINE",argv[3],1);
 void*engine=dlopen(argv[3],RTLD_NOW|RTLD_LOCAL);CHECK(engine);void*bridge=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);CHECK(bridge);
 void(*fixture_mode)(int),(*fixture_arm)(void),(*fixture_wait)(int),(*fixture_release)(int);*(void**)(&fixture_mode)=dlsym(engine,"fixture_mode");*(void**)(&fixture_arm)=dlsym(engine,"fixture_arm");*(void**)(&fixture_wait)=dlsym(engine,"fixture_wait");*(void**)(&fixture_release)=dlsym(engine,"fixture_release");CHECK(fixture_mode&&fixture_arm&&fixture_wait&&fixture_release);
 void(*enabled)(int),(*mode)(int);*(void**)(&enabled)=dlsym(bridge,"clf_bridge_test_set_enabled");*(void**)(&mode)=dlsym(bridge,"clf_bridge_test_set_mode");CHECK(enabled&&mode);
 hw_module_t*module=dlsym(bridge,"HMI");CHECK(module);hw_device_t*common=NULL;CHECK(!module->methods->open(module,"audio_hw_if",&common));audio_hw_device_t*device=(audio_hw_device_t*)common;audio_stream_in_t*stream=NULL;CHECK(!device->open_input_stream(device,1,0x80000004,NULL,&stream,0,"mock",1));CHECK(stream);
 enabled(1);fixture_arm();fixture_mode(2);mode(2);fixture_wait(1);fixture_mode(3);mode(3);fixture_release(1);fixture_wait(2);fixture_mode(2);mode(2);fixture_release(2);
 int matched=0;for(int i=0;i<100&&!matched;i++){FILE*f=tmpfile();CHECK(f);CHECK(!device->dump(device,fileno(f)));fflush(f);rewind(f);char buffer[6000]={0};fread(buffer,1,sizeof(buffer)-1,f);fclose(f);matched=strstr(buffer,"mode=2 backend=dtln requested_mode=2")!=NULL;if(!matched)usleep(20000);}
 CHECK(matched);device->close_input_stream(device,stream);CHECK(!common->close(common));puts("PASS MOCK mode-control 2->3->2 stale build rejected and latest request activated; NOT neural inference proof");return 0;
}
