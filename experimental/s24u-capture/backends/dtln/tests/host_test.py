#!/usr/bin/env python3
"""Exercise actual production C DSP with official LiteRT host interpreters.
The C-ABI shim only substitutes runtime plumbing, not the DSP or neural models.
Android runtime loader/on-device performance remain separate, unverified gates.
"""
import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import numpy as np
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'runtime/host-wheel'))
from ai_edge_litert.interpreter import Interpreter, OpResolverType
BUILD = ROOT/'build/host'
BUILD.mkdir(parents=True,exist_ok=True)
command=['clang','-shared','-fPIC','-std=c11','-O2','-Wall','-Wextra','-Werror',
 '-DCLARIFAE_WITH_DTLN=1',f'-DCLARIFAE_DTLN_MODEL1_PATH="{ROOT}/models/model_1.tflite"',
 f'-DCLARIFAE_DTLN_MODEL2_PATH="{ROOT}/models/model_2.tflite"',
 '-I'+str(ROOT/'runtime/include'),'-I'+str(ROOT/'runtime/include/external/org_tensorflow'),'-I'+str(ROOT/'../../engine'),
 str(ROOT/'../../engine/backend_dtln.c'),str(ROOT/'../../engine/resampler.c'),
 str(ROOT/'tests/host_shim.c'),'-o',str(BUILD/'backend.dylib')]
if os.environ.get('DTLN_UBSAN'):
 command[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all','-g']
subprocess.run(command,check=True)
lib=C.CDLL(str(BUILD/'backend.dylib'))
CALLBACK=C.CFUNCTYPE(C.c_ssize_t,C.c_int,C.c_size_t,C.c_size_t,C.c_size_t)
objects={}; next_id=1; tensors={}; invoked=0; fault=None; bad_contract=None; swapped=False

def save(obj):
 global next_id
 key=next_id;next_id+=1;objects[key]=obj;return key

def make_interpreter(path):
 return Interpreter(model_path=path,num_threads=1,
  experimental_op_resolver_type=OpResolverType.BUILTIN_WITHOUT_DEFAULT_DELEGATES)

def dispatch(op,a,b,c):
 global invoked,fault
 try:
  if fault == op:
   fault=None;return 1
  if op==1:return save(C.string_at(a).decode())
  if op in (2,4,7):
   if op==7:
    for key in [k for k in tensors if k[0]==a]:objects.pop(tensors.pop(key),None)
   objects.pop(a,None);return 0
  if op==3:return save({})
  if op==5:return 0
  if op==6:return save(make_interpreter(objects[a]))
  if op==8:objects[a].allocate_tensors();return 0
  if op in (9,10):return len(objects[a].get_input_details() if op==9 else objects[a].get_output_details())
  if op in (11,12):
   k=(a,op,b)
   if k not in tensors:
    details=objects[a].get_input_details() if op==11 else objects[a].get_output_details()
    item=details[1-b if swapped else b]
    tensors[k]=save((objects[a],item))
   return tensors[k]
  if op==19:objects[a].invoke();invoked+=1;return 0
  inter,detail=objects[a];sh=detail['shape'];n=int(np.prod(sh));size=n*4
  if op==13:return 2 if bad_contract=='type' else 1
  if op==14:return len(sh)+(1 if bad_contract=='rank' else 0)
  if op==15:return int(sh[b])+(1 if bad_contract=='dimension' and b==0 else 0)
  if op==16:return size+(4 if bad_contract=='bytes' else 0)
  if op==17:
   if c!=size:return 1
   arr=np.frombuffer(C.string_at(b,c),dtype=np.float32).reshape(sh).copy()
   inter.set_tensor(detail['index'],arr);return 0
  if op==18:
   if c!=size:return 1
   arr=inter.get_tensor(detail['index']);C.memmove(b,arr.ctypes.data,c);return 0
  raise RuntimeError(op)
 except Exception as e:
  print('shim error',op,repr(e),file=sys.stderr);return 1
callback=CALLBACK(dispatch)
lib.dtln_test_dispatch.argtypes=[CALLBACK];lib.dtln_test_dispatch(callback)
CREATE=C.CFUNCTYPE(C.c_void_p,C.c_int,C.c_int,C.c_void_p,C.c_int)
PROCESS=C.CFUNCTYPE(C.c_int,C.c_void_p,C.POINTER(C.c_float),C.c_int)
PARAMS=C.CFUNCTYPE(None,C.c_void_p,C.c_float,C.c_float,C.c_float,C.c_int,C.c_int)
RESET=C.CFUNCTYPE(None,C.c_void_p);DESTROY=C.CFUNCTYPE(None,C.c_void_p)
class API(C.Structure):
 _fields_=[('name',C.c_char_p),('create',CREATE),('process',PROCESS),('params',PARAMS),('reset',RESET),('destroy',DESTROY)]
lib.clarifae_backend_dtln.restype=C.POINTER(API)
api=lib.clarifae_backend_dtln().contents

def create(rate=16000,ch=1):
 err=C.create_string_buffer(256);s=api.create(rate,ch,err,256)
 assert s,err.value
 return s

def run(x,rate=16000,part=(128,),strength=1,s=None):
 own=s is None
 if own:s=create(rate,x.shape[1] if x.ndim==2 else 1)
 api.params(s,strength,24,.5,0,0)
 y=x.astype(np.float32).copy();offset=0;j=0
 while offset<len(y):
  n=min(part[j%len(part)],len(y)-offset);buf=y[offset:offset+n]
  rc=api.process(s,buf.ctypes.data_as(C.POINTER(C.c_float)),n)
  assert rc==0,(rate,offset,rc)
  offset+=n;j+=1
 if own:api.destroy(s)
 return y

def reference(x):
 a=make_interpreter(str(ROOT/'models/model_1.tflite'));b=make_interpreter(str(ROOT/'models/model_2.tflite'))
 a.allocate_tensors();b.allocate_tensors();ad=a.get_input_details();ao=a.get_output_details();bd=b.get_input_details();bo=b.get_output_details()
 sa=np.zeros((1,2,128,2),np.float32);sb=sa.copy();window=np.zeros(512,np.float32);ola=np.zeros(512,np.float32);out=[]
 for start in range(0,len(x)-127,128):
  window[:-128]=window[128:];window[-128:]=x[start:start+128]/32768
  spectrum=np.fft.rfft(window);mag=np.abs(spectrum).reshape(1,1,257).astype(np.float32)
  a.set_tensor(ad[0]['index'],mag);a.set_tensor(ad[1]['index'],sa);a.invoke()
  mask=a.get_tensor(ao[0]['index']);sa=a.get_tensor(ao[1]['index'])
  estimate=np.fft.irfft(mag*mask*np.exp(1j*np.angle(spectrum))).astype(np.float32).reshape(1,1,512)
  b.set_tensor(bd[0]['index'],estimate);b.set_tensor(bd[1]['index'],sb);b.invoke()
  enhanced=b.get_tensor(bo[0]['index']).reshape(512);sb=b.get_tensor(bo[1]['index'])
  ola[:-128]=ola[128:];ola[-128:]=0;ola+=enhanced;out.extend(ola[:128]*32768)
 return np.concatenate([np.zeros(128,np.float32),out])[:len(x)]

report={'host_runtime':'official ai-edge-litert 1.2.0 macOS arm64 Python wheel; test-only C-ABI bridge', 'tests':[]}
rng=np.random.default_rng(827)
x=(5000*np.sin(2*np.pi*440*np.arange(16000)/16000)+rng.normal(0,500,16000)).astype(np.float32)
start=time.perf_counter();y=run(x);elapsed=time.perf_counter()-start
r=reference(x);error=float(np.max(np.abs(y-r)));assert error<.5,error
report['reference_max_error_int16_units']=error;report['one_second_create_and_process_wall_seconds']=elapsed
report['tests'].append('C backend matches official normalized two-stage NumPy recipe')
for rate,ch in [(8000,1),(16000,1),(44100,1),(48000,2),(96000,1),(192000,1)]:
 n=rate//4+99;t=np.arange(n)/rate
 v=np.stack([6000*np.sin(2*np.pi*(190+c*70)*t)+rng.normal(0,90,n) for c in range(ch)],axis=-1).astype(np.float32)
 yy=run(v,rate,(128,));zz=run(v,rate,(1,7,503,8193,2,71))
 assert np.array_equal(yy,zz),(rate,np.max(np.abs(yy-zz)))
 assert np.isfinite(yy).all()
 report['tests'].append(f'{rate} Hz {ch}ch bit-exact fixed/irregular callback partitions incl one-sample calls')
# Delayed dry is original PCM, not downmix or NN output, no disabled clamp.
for rate in [8000,16000,44100,48000,192000]:
 n=max(rate//8,4096);v=rng.normal(0,40000,(n,2)).astype(np.float32)
 z=run(v,rate,(1,351,17),strength=0)
 priming=128 if rate==16000 else (128*rate+15999)//16000+2
 delay=priming+384*rate/16000;d=int(np.floor(delay));f=np.float32(delay-d)
 expected=np.zeros_like(v)
 if d<n:
  expected[d:]=v[:n-d]
  if f:
   prev=np.zeros_like(expected[d:]);prev[1:]=v[:n-d-1]
   expected[d:]=expected[d:]+(prev-expected[d:])*f
 assert np.max(np.abs(z-expected)) < (.03 if f else 1e-20),(rate,np.max(np.abs(z-expected)))
 report['tests'].append(f'{rate} Hz stereo strength=0 delayed dry (exact for integer delay), latency {delay:g} samples')
# Per-channel stereo equals independent mono, with silent right channel.
v=np.stack([x,np.zeros_like(x)],axis=-1)
z=run(v);left=run(x);right=run(np.zeros_like(x))
assert np.array_equal(z[:,0],left) and np.array_equal(z[:,1],right)
report['tests'].append('independent stereo states, no downmix/fanout')
s=create();run(x[:6000],s=s);api.reset(s);z=run(x,s=s);api.destroy(s)
assert np.array_equal(z,y)
report['tests'].append('reset equals fresh instance after nonzero neural history')
# Shape classification is not index-based; wrong schemas fail at creation.
swapped=True;s=create();api.destroy(s);swapped=False
for bad_contract in ['type','rank','dimension','bytes']:
 err=C.create_string_buffer(256);s=api.create(16000,1,err,256);assert not s
bad_contract=None
report['tests'].append('swapped tensor indexes accepted; type/rank/dim/bytes mismatch rejected')
# All copy and invoke failures latch EIO and zero the entire callback.
for opcode in [17,18,19]:
 s=create();fault=opcode;buf=x[:512].copy()
 rc=api.process(s,buf.ctypes.data_as(C.POINTER(C.c_float)),len(buf));assert rc<0 and not buf.any()
 count=invoked;buf[:]=1
 rc=api.process(s,buf.ctypes.data_as(C.POINTER(C.c_float)),len(buf));assert rc<0 and not buf.any() and invoked==count
 api.reset(s);z=run(x,s=s);assert np.array_equal(z,y);api.destroy(s)
report['tests'].append('copy-in/copy-out/invoke errors: silent fail, sticky until clean reset')
s=create();buf=np.array([np.nan],np.float32);assert api.process(s,buf.ctypes.data_as(C.POINTER(C.c_float)),1)<0 and buf[0]==0;api.destroy(s)
report['tests'].append('nonfinite input rejected and zeroed')
# Long callback exceeds every fixed scratch/ring capacity; no bulk FIFO overflow.
z=run(x,(16000),(16000,));assert np.array_equal(z,y)
report['tests'].append('single 16000-frame callback equals fixed 128 framing')
# C DSP must have no TFLite creation/alloc calls in process/reset. The Python
# binding is not suitable for measuring native allocations or realtime safety.

# Model positional delay (384 samples) plus fixed hop priming (128 samples).
# Nonlinear learned filters may move an impulse peak; record rather than invent.
impulse=np.zeros(8192,np.float32);impulse[2048]=16000
response=run(impulse);peak=int(np.argmax(np.abs(response)))
report['impulse']={'input_index':2048,'output_peak_index':peak,'peak_lag_samples':peak-2048,'peak_int16_units':float(np.max(np.abs(response))),'nominal_alignment_delay_samples':512}
d=run(x,strength=0);m=run(x,strength=.37)
expected_mix=d+np.float32(.37)*(y-d)
assert np.max(np.abs(m-expected_mix))<.002
report['tests'].append('strength=.37 follows aligned wet/dry mix without level-floor distortion')
# Warm actual host runtime C++ kernels, without C-ABI callback overhead.
a=make_interpreter(str(ROOT/'models/model_1.tflite'));b=make_interpreter(str(ROOT/'models/model_2.tflite'))
for it in [a,b]:
 it.allocate_tensors()
 for t in it.get_input_details():it.set_tensor(t['index'],np.zeros(t['shape'],np.float32))
 it.invoke()
t0=time.process_time()
for k in range(500):a.invoke();b.invoke()
sec=time.process_time()-t0
report['host_kernel_cpu']={'pairs':500,'cpu_seconds':sec,'milliseconds_per_8ms_pair':sec*2,'realtime_cpu_fraction_one_channel':sec/4,'caveat':'M4 host builtins, not phone CPU; zero fixed input, no FFT/SRC or Python C-ABI bridge'}
for rate in [11025,22050,32000,47999]:
 v=rng.normal(0,1000,4097).astype(np.float32)
 aout=run(v,rate,(1,));bout=run(v,rate,(4097,));assert np.array_equal(aout,bout)
report['tests'].append('11025/22050/32000/47999 Hz long-vs-single sample partition invariance')
report['signal']={'input_rms':float(np.sqrt(np.mean(x*x))), 'wet_rms':float(np.sqrt(np.mean(y*y))), 'wet_peak':float(np.max(np.abs(y))), 'all_finite':bool(np.isfinite(y).all())}
report['passed']=len(report['tests'])
print(json.dumps(report,indent=2))
(ROOT/'tests/host-results.json').write_text(json.dumps(report,indent=2)+chr(10))