/* TEST ONLY: forwards the C ABI to official ai-edge-litert Python bindings.
 * This is not a production runtime and MUST NOT ship in the Android module. */
#include "tflite/c/c_api.h"
#include <stdint.h>
typedef intptr_t (*dispatcher_t)(int,uintptr_t,uintptr_t,uintptr_t);
static dispatcher_t dispatch;
void dtln_test_dispatch(dispatcher_t fn) {dispatch=fn;}
#define D(op,a,b,c) dispatch(op,(uintptr_t)(a),(uintptr_t)(b),(uintptr_t)(c))
TfLiteModel *TfLiteModelCreateFromFile(const char *p){return (TfLiteModel*)D(1,p,0,0);}
void TfLiteModelDelete(TfLiteModel *m){D(2,m,0,0);}
TfLiteInterpreterOptions *TfLiteInterpreterOptionsCreate(void){return (TfLiteInterpreterOptions*)D(3,0,0,0);}
void TfLiteInterpreterOptionsDelete(TfLiteInterpreterOptions *o){D(4,o,0,0);}
void TfLiteInterpreterOptionsSetNumThreads(TfLiteInterpreterOptions *o,int n){D(5,o,n,0);}
TfLiteInterpreter *TfLiteInterpreterCreate(const TfLiteModel *m,const TfLiteInterpreterOptions *o){return (TfLiteInterpreter*)D(6,m,o,0);}
void TfLiteInterpreterDelete(TfLiteInterpreter *i){D(7,i,0,0);}
TfLiteStatus TfLiteInterpreterAllocateTensors(TfLiteInterpreter *i){return (TfLiteStatus)D(8,i,0,0);}
int32_t TfLiteInterpreterGetInputTensorCount(const TfLiteInterpreter *i){return (int32_t)D(9,i,0,0);}
int32_t TfLiteInterpreterGetOutputTensorCount(const TfLiteInterpreter *i){return (int32_t)D(10,i,0,0);}
TfLiteTensor *TfLiteInterpreterGetInputTensor(const TfLiteInterpreter *i,int32_t n){return (TfLiteTensor*)D(11,i,n,0);}
const TfLiteTensor *TfLiteInterpreterGetOutputTensor(const TfLiteInterpreter *i,int32_t n){return (TfLiteTensor*)D(12,i,n,0);}
TfLiteType TfLiteTensorType(const TfLiteTensor *t){return (TfLiteType)D(13,t,0,0);}
int32_t TfLiteTensorNumDims(const TfLiteTensor *t){return (int32_t)D(14,t,0,0);}
int32_t TfLiteTensorDim(const TfLiteTensor *t,int32_t n){return (int32_t)D(15,t,n,0);}
size_t TfLiteTensorByteSize(const TfLiteTensor *t){return (size_t)D(16,t,0,0);}
TfLiteStatus TfLiteTensorCopyFromBuffer(TfLiteTensor *t,const void *p,size_t n){return (TfLiteStatus)D(17,t,p,n);}
TfLiteStatus TfLiteTensorCopyToBuffer(const TfLiteTensor *t,void *p,size_t n){return (TfLiteStatus)D(18,t,p,n);}
TfLiteStatus TfLiteInterpreterInvoke(TfLiteInterpreter *i){return (TfLiteStatus)D(19,i,0,0);}
