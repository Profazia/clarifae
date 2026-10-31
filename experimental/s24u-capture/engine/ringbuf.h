/*
 * ringbuf.h — single-producer/single-consumer float ring buffer.
 * Used to align AudioFlinger's arbitrary process() sizes to the fixed block
 * size a backend needs (e.g. RNNoise's 480-sample frames). All access happens
 * on the one audio thread, so no locking is required.
 */
#ifndef CLARIFAE_RINGBUF_H
#define CLARIFAE_RINGBUF_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ringbuf {
    float* buf;
    int    cap;    /* capacity in samples */
    int    head;   /* read index  */
    int    tail;   /* write index */
    int    count;  /* samples currently stored */
} ringbuf_t;

int  rb_init(ringbuf_t* rb, int cap);   /* 0 on success, -1 on alloc failure */
void rb_free(ringbuf_t* rb);
void rb_reset(ringbuf_t* rb);
int  rb_avail(const ringbuf_t* rb);     /* samples available to read */
int  rb_space(const ringbuf_t* rb);     /* free space for writing */
int  rb_write(ringbuf_t* rb, const float* src, int n); /* returns samples written */
int  rb_read(ringbuf_t* rb, float* dst, int n);        /* returns samples read */

#ifdef __cplusplus
}
#endif

#endif /* CLARIFAE_RINGBUF_H */
