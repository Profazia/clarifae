//! Bounded Clarifae C ABI, not the unsafe upstream capi ABI.
//! No global logger, file access only in create. Same model/DSP as official libDF.
use df_core::tract::{DfParams, DfTract, ReduceMask, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};
use sha2::{Digest, Sha256};
use std::ffi::{c_char, c_float, CStr};
use std::fs::File;
use std::io::Read;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

const EINVAL: i32 = -22;
const EIO: i32 = -5;
const EPANIC: i32 = -14;
const HOP: usize = 480;
const MODEL_BYTES: usize = 7_983_136;
const MODEL_SHA: [u8; 32] = [
    0xc9,0x4d,0x91,0xf7,0x09,0x11,0x00,0x1c,0x94,0x6e,0x0f,0xab,0xb4,0xaa,0x9a,0xdc,
    0x37,0x04,0x5f,0x45,0xa0,0x3b,0x56,0x00,0x8c,0xb0,0xc8,0x24,0x4c,0xb6,0x36,0x16,
];

#[cfg(feature = "allocation-metrics")]
mod alloc_metrics {
    use std::alloc::{GlobalAlloc, Layout, System};
    use std::sync::atomic::{AtomicU64, Ordering};
    pub static CALLS: AtomicU64 = AtomicU64::new(0);
    pub struct Measured;
    unsafe impl GlobalAlloc for Measured {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 { CALLS.fetch_add(1, Ordering::Relaxed); System.alloc(l) }
        unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 { CALLS.fetch_add(1, Ordering::Relaxed); System.alloc_zeroed(l) }
        unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 { CALLS.fetch_add(1, Ordering::Relaxed); System.realloc(p,l,n) }
        unsafe fn dealloc(&self, p: *mut u8, l: Layout) { System.dealloc(p,l) }
    }
}
#[cfg(feature = "allocation-metrics")]
#[global_allocator]
static ALLOCATOR: alloc_metrics::Measured = alloc_metrics::Measured;

pub struct CdfState {
    m: DfTract,
    pristine: DfTract,
    atten_db: f32,
    poisoned: bool,
    frames: u64,
    #[cfg(test)]
    panic_next_process: bool,
}

unsafe fn error(buf: *mut c_char, cap: usize, msg: &'static str) {
    if !buf.is_null() && cap > 0 {
        let n = (cap - 1).min(msg.len());
        ptr::copy_nonoverlapping(msg.as_ptr(), buf.cast::<u8>(), n);
        *buf.add(n) = 0;
    }
}

fn create_inner(path: &CStr, atten: f32) -> Result<CdfState, &'static str> {
    let path = path.to_str().map_err(|_| "model path is not UTF-8")?;
    if !atten.is_finite() || !(0.0..=100.0).contains(&atten) { return Err("invalid attenuation"); }
    let mut f = File::open(path).map_err(|_| "cannot open pinned DeepFilterNet3 model")?;
    if f.metadata().map_err(|_| "cannot stat model")?.len() != MODEL_BYTES as u64 {
        return Err("model size mismatch");
    }
    let mut bytes = Vec::with_capacity(MODEL_BYTES + 1);
    (&mut f).take((MODEL_BYTES + 1) as u64).read_to_end(&mut bytes).map_err(|_| "model read failed")?;
    if bytes.len() != MODEL_BYTES || Sha256::digest(&bytes).as_slice() != MODEL_SHA {
        return Err("model SHA-256 mismatch");
    }
    let dfp = DfParams::from_bytes(&bytes).map_err(|_| "model archive parse failed")?;
    let rp = RuntimeParams::default_with_ch(1)
        .with_atten_lim(atten).with_thresholds(-15.0,35.0,35.0)
        .with_post_filter(0.0).with_mask_reduce(ReduceMask::MAX);
    let m = DfTract::new(dfp, &rp).map_err(|_| "DeepFilterNet3 tract initialization failed")?;
    if m.sr != 48000 || m.hop_size != HOP || m.fft_size != 960 || m.lookahead != 2 || m.df_order != 5 {
        return Err("unexpected model geometry");
    }
    // Clone captures all graph/GRU/convolution/norm/STFT state before any frame.
    // Upstream init() is NOT a reset: it misses recurrent state and appends x FIFO.
    let pristine = m.clone();
    Ok(CdfState { m, pristine, atten_db: atten, poisoned: false, frames: 0,
        #[cfg(test)] panic_next_process: false })
}

#[no_mangle]
pub unsafe extern "C" fn clarifae_df_create(path: *const c_char, atten_db: c_float,
    err: *mut c_char, err_cap: usize) -> *mut CdfState {
    error(err,err_cap,"");
    if path.is_null() { error(err,err_cap,"null model path"); return ptr::null_mut(); }
    // Caller must provide a valid NUL-terminated C string; reject >4095 bytes.
    let mut n = 0;
    while n < 4096 && *path.add(n) != 0 { n += 1; }
    if n == 4096 { error(err,err_cap,"model path too long"); return ptr::null_mut(); }
    match catch_unwind(AssertUnwindSafe(|| create_inner(CStr::from_ptr(path),atten_db))) {
        Ok(Ok(s)) => Box::into_raw(Box::new(s)),
        Ok(Err(e)) => { error(err,err_cap,e); ptr::null_mut() },
        Err(p) => { std::mem::forget(p); error(err,err_cap,"caught Rust model initialization panic"); ptr::null_mut() },
    }
}

#[no_mangle]
pub unsafe extern "C" fn clarifae_df_process(st: *mut CdfState, input: *const f32,
    input_len: usize, output: *mut f32, output_len: usize, lsnr: *mut f32) -> i32 {
    if st.is_null() || input.is_null() || output.is_null() || input_len != HOP || output_len != HOP {
        return EINVAL;
    }
    let a = input as usize; let b = output as usize; let bytes = HOP * std::mem::size_of::<f32>();
    if (a | b) % std::mem::align_of::<f32>() != 0 || a.abs_diff(b) < bytes { return EINVAL; }
    if !lsnr.is_null() {
        let p=lsnr as usize;
        if p % std::mem::align_of::<f32>() != 0 || p.wrapping_sub(a) < bytes || p.wrapping_sub(b) < bytes { return EINVAL; }
    } // all input/output/SNR views must be non-aliasing
    let s = &mut *st;
    let out = std::slice::from_raw_parts_mut(output,HOP);
    out.fill(0.0);
    if s.poisoned { return EIO; }
    let inp = std::slice::from_raw_parts(input,HOP);
    if inp.iter().any(|x| !x.is_finite() || x.abs() > 1.0) { return EINVAL; }
    let result = catch_unwind(AssertUnwindSafe(|| {
        #[cfg(test)]
        if std::mem::take(&mut s.panic_next_process) { panic!("injected DSP panic"); }
        s.m.process(ArrayView2::from_shape((1,HOP),inp).unwrap(),
                    ArrayViewMut2::from_shape((1,HOP),&mut *out).unwrap())
    }));
    match result {
        Ok(Ok(snr)) if snr.is_finite() && out.iter().all(|x| x.is_finite()) => {
            if !lsnr.is_null() { *lsnr = snr; }
            s.frames = s.frames.saturating_add(1);
            0
        },
        Ok(_) => { out.fill(0.0); s.poisoned = true; EIO },
        Err(p) => { std::mem::forget(p); out.fill(0.0); s.poisoned = true; EPANIC },
    }
}

#[no_mangle]
pub unsafe extern "C" fn clarifae_df_reset(st: *mut CdfState) -> i32 {
    if st.is_null() { return EINVAL; }
    let s = &mut *st;
    match catch_unwind(AssertUnwindSafe(|| {
        let mut fresh = s.pristine.clone(); // allocation: control worker only
        fresh.set_atten_lim(s.atten_db);
        s.m = fresh;
        s.poisoned = false;
        s.frames = 0;
    })) {
        Ok(()) => 0,
        Err(p) => { std::mem::forget(p); s.poisoned = true; EPANIC },
    }
}

#[no_mangle]
pub unsafe extern "C" fn clarifae_df_set_atten_lim(st: *mut CdfState, db: f32) -> i32 {
    if st.is_null() || !db.is_finite() || !(0.0..=100.0).contains(&db) { return EINVAL; }
    let s = &mut *st;
    match catch_unwind(AssertUnwindSafe(|| { s.m.set_atten_lim(db); s.atten_db = db; })) {
        Ok(()) => 0,
        Err(p) => { std::mem::forget(p); s.poisoned = true; EPANIC },
    }
}

#[no_mangle]
pub unsafe extern "C" fn clarifae_df_frame_length(st: *const CdfState) -> usize { if st.is_null() {0} else {HOP} }
#[no_mangle]
pub unsafe extern "C" fn clarifae_df_sample_rate(st: *const CdfState) -> usize { if st.is_null() {0} else {48000} }
#[no_mangle]
pub unsafe extern "C" fn clarifae_df_delay_samples(st: *const CdfState) -> usize {
    if st.is_null() {0} else { let s = &*st; if s.atten_db < 0.01 { 0 } else { s.m.fft_size - s.m.hop_size + s.m.lookahead * s.m.hop_size } }
}
#[no_mangle]
pub unsafe extern "C" fn clarifae_df_processed_frames(st: *const CdfState) -> u64 { if st.is_null() {0} else {(*st).frames} }
#[no_mangle]
pub extern "C" fn clarifae_df_backend_name() -> *const c_char { b"DeepFilterNet3/tract-0.21.4/clarifae-checked-v1\0".as_ptr().cast() }
#[no_mangle]
pub extern "C" fn clarifae_df_allocation_count() -> u64 {
    #[cfg(feature = "allocation-metrics")]
    { return alloc_metrics::CALLS.load(std::sync::atomic::Ordering::Relaxed); }
    #[cfg(not(feature = "allocation-metrics"))]
    { u64::MAX }
}
#[no_mangle]
pub unsafe extern "C" fn clarifae_df_free(st: *mut CdfState) {
    if !st.is_null() {
        if let Err(p) = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(st)))) { std::mem::forget(p); }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn ffi_contains_internal_panic_and_reset_recovers() {
        let path = std::ffi::CString::new(concat!(env!("CARGO_MANIFEST_DIR"),"/../models/DeepFilterNet3_onnx.tar.gz")).unwrap();
        let mut err=[0i8;128];
        unsafe {
            let st=super::clarifae_df_create(path.as_ptr(),100.0,err.as_mut_ptr(),err.len());
            assert!(!st.is_null());
            // Test-only fault branch inside the real C process boundary;
            // compiled out of every production library.
            (*st).panic_next_process=true;
            let input=[0.125f32;480]; let mut output=[1.0f32;480];
            assert_eq!(super::clarifae_df_process(st,input.as_ptr(),480,output.as_mut_ptr(),480,std::ptr::null_mut()),-14);
            assert!(output.iter().all(|&x| x==0.0));
            assert_eq!(super::clarifae_df_process(st,input.as_ptr(),480,output.as_mut_ptr(),480,std::ptr::null_mut()),-5);
            assert_eq!(super::clarifae_df_reset(st),0);
            assert_eq!(super::clarifae_df_process(st,input.as_ptr(),480,output.as_mut_ptr(),480,std::ptr::null_mut()),0);
            super::clarifae_df_free(st);
        }
    }
}
