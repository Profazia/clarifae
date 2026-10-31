#include "ringbuf.h"

#include <stdlib.h>
#include <string.h>

int rb_init(ringbuf_t* rb, int cap) {
    rb->buf = (float*)malloc((size_t)cap * sizeof(float));
    if (!rb->buf) return -1;
    rb->cap = cap;
    rb->head = rb->tail = rb->count = 0;
    return 0;
}

void rb_free(ringbuf_t* rb) {
    free(rb->buf);
    rb->buf = NULL;
    rb->cap = rb->head = rb->tail = rb->count = 0;
}

void rb_reset(ringbuf_t* rb) {
    rb->head = rb->tail = rb->count = 0;
}

int rb_avail(const ringbuf_t* rb) { return rb->count; }
int rb_space(const ringbuf_t* rb) { return rb->cap - rb->count; }

int rb_write(ringbuf_t* rb, const float* src, int n) {
    int space = rb->cap - rb->count;
    if (n > space) n = space;
    for (int i = 0; i < n; i++) {
        rb->buf[rb->tail] = src[i];
        rb->tail = (rb->tail + 1) % rb->cap;
    }
    rb->count += n;
    return n;
}

int rb_read(ringbuf_t* rb, float* dst, int n) {
    if (n > rb->count) n = rb->count;
    for (int i = 0; i < n; i++) {
        dst[i] = rb->buf[rb->head];
        rb->head = (rb->head + 1) % rb->cap;
    }
    rb->count -= n;
    return n;
}
