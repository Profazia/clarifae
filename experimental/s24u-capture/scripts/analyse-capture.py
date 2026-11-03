#!/usr/bin/env python3
"""Analyse the documented 24s OFF->ON->OFF laboratory capture; no network/device I/O."""
import argparse,json
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser();p.add_argument('pcm');p.add_argument('--output');a=p.parse_args()
x=np.fromfile(a.pcm,dtype='<i2').astype(float)/32768;sr=48000
out={'sample_rate':sr,'duration_seconds':len(x)/sr,'scope':'Documented single real-mic 24s OFF/ON/OFF protocol; 4–7.5kHz noise band, not speech SNR.','segments':{}}
for name,(lo,hi) in {'off_before':(2,5),'on':(8,12),'off_after':(17,21)}.items():
 s=x[int(lo*sr):int(hi*sr)];n=4096;w=np.hanning(n);frames=np.stack([s[i:i+n] for i in range(0,len(s)-n+1,n//2)]);frames-=frames.mean(axis=1,keepdims=True)
 power=np.mean(abs(np.fft.rfft(frames*w,axis=1))**2,axis=0);freq=np.fft.rfftfreq(n,1/sr)
 out['segments'][name]={'interval_seconds':[lo,hi],'rms_dbfs':float(20*np.log10(max(np.sqrt(np.mean(s*s)),1e-12))),'noise_4k_7p5k_power':float(np.sum(power[(freq>=4000)&(freq<=7500)])),'clipped_samples':int(np.count_nonzero(abs(s)>=32767/32768))}
for name in ['off_before','off_after']:out[name+'_to_on_noise_band_reduction_db']=float(10*np.log10(out['segments'][name]['noise_4k_7p5k_power']/max(out['segments']['on']['noise_4k_7p5k_power'],1e-24)))
text=json.dumps(out,indent=2)+'\n'
if a.output:Path(a.output).write_text(text)
print(text,end='')
