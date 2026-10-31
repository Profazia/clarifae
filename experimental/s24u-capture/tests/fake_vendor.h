/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CLARIFAE_FAKE_VENDOR_H
#define CLARIFAE_FAKE_VENDOR_H
#include "../include/audio_hal_abi.h"

#if defined(__GNUC__)
#define FAKE_API __attribute__((visibility("default")))
#else
#define FAKE_API
#endif
#if defined(__ELF__) && defined(__GNUC__)
/* Ensure fixture-local &HMI cannot accidentally resolve to wrapper HMI. */
#define FAKE_HMI_API __attribute__((visibility("protected")))
#else
#define FAKE_HMI_API FAKE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_MAX_DEVICES 4
#define FAKE_MAX_STREAMS 16
#define FAKE_TEXT_CAPACITY 256
#define FAKE_DEFAULT_RATE 48000u
#define FAKE_DEFAULT_CHANNELS AUDIO_CHANNEL_IN_MONO
#define FAKE_DEFAULT_FORMAT AUDIO_FORMAT_PCM_16_BIT
#define FAKE_DEFAULT_SOURCE AUDIO_SOURCE_MIC
#define FAKE_DEFAULT_SEED UINT32_C(0x13579bdf)

struct fake_device_open_args {
    const struct hw_module_t *module;
    const char *id;                 /* exact original pointer; may later expire */
    struct hw_device_t **device_out;
    char id_copy[FAKE_TEXT_CAPACITY];
};
struct fake_input_open_args {
    struct audio_hw_device *device;
    audio_io_handle_t handle;
    audio_devices_t devices;
    struct audio_config *config;    /* opaque: captured, NEVER dereferenced */
    struct audio_stream_in **stream_out;
    audio_input_flags_t flags;
    const char *address;            /* exact original pointer; may later expire */
    audio_source_t source;
    char address_copy[FAKE_TEXT_CAPACITY];
};
struct fake_stream_state {
    struct audio_stream_in *stream;
    struct audio_hw_device *owner;
    uint32_t rate;
    audio_channel_mask_t channels;
    audio_format_t format;
    audio_devices_t devices;
    audio_source_t source;
    bool live;
    bool in_standby;
    uint64_t read_calls, standby_calls, dump_calls, parameter_calls, close_calls;
    uint64_t add_effect_calls, remove_effect_calls;
    uint64_t byte_cursor;
    size_t last_requested, last_written;
    ssize_t last_result;
    int last_dump_fd;
    const char *last_parameters;    /* original pointer, never retained for dereference */
    effect_handle_t last_effect;
    char parameters_copy[FAKE_TEXT_CAPACITY];
};
struct fake_state {
    uint64_t device_open_calls, input_open_calls, device_close_calls, input_close_calls;
    uint64_t read_calls, standby_calls, device_dump_calls, stream_dump_calls;
    uint64_t device_parameter_calls, stream_parameter_calls;
    uint64_t invalid_pointer_calls, integrity_failures;
    size_t live_devices, live_streams;
    struct fake_device_open_args last_device_open;
    struct fake_input_open_args last_input_open;
    struct audio_hw_device *last_device;
    struct audio_stream_in *last_stream;
    const void *last_receiver;
    void *last_read_buffer;
    size_t last_requested, last_written;
    ssize_t last_read_result;
    int last_device_dump_fd, last_stream_dump_fd;
    effect_handle_t last_added_effect, last_removed_effect;
    char device_parameters_copy[FAKE_TEXT_CAPACITY];
};

FAKE_HMI_API extern struct audio_module HMI;
FAKE_API const struct hw_module_t *fake_module(void);
FAKE_API const struct fake_state *fake_get_state(void);
FAKE_API int fake_get_stream_state(const struct audio_stream_in *stream,
                                   struct fake_stream_state *out);
/* Single-threaded controls. Reset only after adapter closes all live objects;
 * reset invalidates old objects; it does not reset adapter-owned bookkeeping.
 */
FAKE_API void fake_reset(void);
/* Parent-added lease and synchronized read/close fixtures. */
FAKE_API void fake_set_singleton_mode(int enabled);
FAKE_API void fake_set_standby_unblocks(int enabled);
FAKE_API void fake_block_reads(int blocked);
FAKE_API void fake_wait_read_entered(void);
FAKE_API void fake_set_open_device_result(int result); /* persistent; zero succeeds */
FAKE_API void fake_set_open_input_result(int result);  /* persistent; zero succeeds */
FAKE_API void fake_set_config(uint32_t rate, audio_channel_mask_t channels,
                              audio_format_t format); /* future opens only */
FAKE_API int fake_set_stream_config(struct audio_stream_in *stream, uint32_t rate,
                                    audio_channel_mask_t channels, audio_format_t format);
FAKE_API void fake_set_seed(uint32_t seed);           /* future opens only */
FAKE_API int fake_rewind_stream(struct audio_stream_in *stream, uint32_t seed);
/* Default reads return requested bytes. Forced nonnegative results write only
 * min(result, requested), even for oversized results; negative results write none.
 * One-shot takes precedence over persistent; use e.g. -EIO, 0, 7, bytes+1.
 */
FAKE_API void fake_set_read_result(ssize_t result);
FAKE_API void fake_set_next_read_result(ssize_t result);
FAKE_API void fake_set_read_default(void);            /* clears both overrides */
FAKE_API void fake_set_standby_result(int result);
FAKE_API void fake_set_parameters_result(int result); /* device + stream */
FAKE_API void fake_set_dump_results(int device_result, int stream_result);
FAKE_API void fake_set_close_device_result(int result);
/* Checks reserved fields, private canaries and all callbacks EXCEPT the eight
 * slots the parent is authorized to patch. Returns 0 or -EFAULT. No real I/O.
 */
FAKE_API int fake_check_integrity(void);
#ifdef __cplusplus
}
#endif
#endif
