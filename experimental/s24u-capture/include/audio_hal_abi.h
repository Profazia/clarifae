/* SPDX-License-Identifier: Apache-2.0
 * Minimal LP64 prefixes of AOSP hardware.h/audio.h, verified against the
 * pinned refs and pineapple stock ELF. These are NOT complete audio structs:
 * never allocate/copy a real vendor object using these prefix sizes.
 */
#ifndef CLARIFAE_AUDIO_HAL_ABI_H
#define CLARIFAE_AUDIO_HAL_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "../engine/include/audio_effect.h"

#if UINTPTR_MAX != UINT64_MAX
#error "The audited audio HAL ABI is LP64 only"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Scalar wire aliases: exact widths/calling convention, not the full AOSP
 * generated enums. Do not infer cross-DSO CFI type identity from these aliases.
 */
typedef uint32_t audio_channel_mask_t;
typedef uint32_t audio_format_t;
typedef uint32_t audio_devices_t;
typedef uint32_t audio_output_flags_t;
typedef uint32_t audio_input_flags_t;
typedef int32_t audio_io_handle_t;
typedef int32_t audio_mode_t;
typedef int32_t audio_source_t;

struct audio_config;
struct audio_stream_out;
struct audio_microphone_characteristic_t;
struct hw_module_t;
struct hw_device_t;

#define HARDWARE_MODULE_TAG UINT32_C(0x48574d54)
#define HARDWARE_DEVICE_TAG UINT32_C(0x48574454)
#define HARDWARE_MAKE_API_VERSION(major, minor) \
    ((((major) & 0xff) << 8) | ((minor) & 0xff))
#define HARDWARE_HAL_API_VERSION HARDWARE_MAKE_API_VERSION(1, 0)
#define AUDIO_MODULE_API_VERSION_0_1 HARDWARE_MAKE_API_VERSION(0, 1)
#define AUDIO_MODULE_API_VERSION_CURRENT AUDIO_MODULE_API_VERSION_0_1
#define AUDIO_DEVICE_API_VERSION_3_2 HARDWARE_MAKE_API_VERSION(3, 2)
#define AUDIO_HARDWARE_MODULE_ID "audio"
#define AUDIO_HARDWARE_INTERFACE "audio_hw_if"
#define HAL_MODULE_INFO_SYM HMI
#define HAL_MODULE_INFO_SYM_AS_STR "HMI"
#define AUDIO_SOURCE_MIC ((audio_source_t)1)
#define AUDIO_SOURCE_VOICE_COMMUNICATION ((audio_source_t)7)

typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id,
                struct hw_device_t **device);
} hw_module_methods_t;

typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint64_t reserved[32 - 7];
} hw_module_t;

typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t *device);
} hw_device_t;

struct audio_module {
    struct hw_module_t common;
};

struct audio_stream {
    uint32_t (*get_sample_rate)(const struct audio_stream *stream);
    int (*set_sample_rate)(struct audio_stream *stream, uint32_t rate);
    size_t (*get_buffer_size)(const struct audio_stream *stream);
    audio_channel_mask_t (*get_channels)(const struct audio_stream *stream);
    audio_format_t (*get_format)(const struct audio_stream *stream);
    int (*set_format)(struct audio_stream *stream, audio_format_t format);
    int (*standby)(struct audio_stream *stream);
    int (*dump)(const struct audio_stream *stream, int fd);
    audio_devices_t (*get_device)(const struct audio_stream *stream);
    int (*set_device)(struct audio_stream *stream, audio_devices_t device);
    int (*set_parameters)(struct audio_stream *stream, const char *kv_pairs);
    char *(*get_parameters)(const struct audio_stream *stream, const char *keys);
    int (*add_audio_effect)(const struct audio_stream *stream, effect_handle_t effect);
    int (*remove_audio_effect)(const struct audio_stream *stream, effect_handle_t effect);
};
typedef struct audio_stream audio_stream_t;

/* PREFIX ONLY. The real get_input_frames_lost slot starts at byte 128 and
 * get_capture_position at 136. This adapter does not access either or the tail.
 */
struct audio_stream_in {
    struct audio_stream common;
    int (*set_gain)(struct audio_stream_in *stream, float gain);
    ssize_t (*read)(struct audio_stream_in *stream, void *buffer, size_t bytes);
};
typedef struct audio_stream_in audio_stream_in_t;

/* PREFIX ONLY: all slots after dump belong to the original vendor object. */
struct audio_hw_device {
    struct hw_device_t common;
    uint32_t (*get_supported_devices)(const struct audio_hw_device *dev);
    int (*init_check)(const struct audio_hw_device *dev);
    int (*set_voice_volume)(struct audio_hw_device *dev, float volume);
    int (*set_master_volume)(struct audio_hw_device *dev, float volume);
    int (*get_master_volume)(struct audio_hw_device *dev, float *volume);
    int (*set_mode)(struct audio_hw_device *dev, audio_mode_t mode);
    int (*set_mic_mute)(struct audio_hw_device *dev, bool state);
    int (*get_mic_mute)(const struct audio_hw_device *dev, bool *state);
    int (*set_parameters)(struct audio_hw_device *dev, const char *kv_pairs);
    char *(*get_parameters)(const struct audio_hw_device *dev, const char *keys);
    size_t (*get_input_buffer_size)(const struct audio_hw_device *dev,
                                    const struct audio_config *config);
    int (*open_output_stream)(struct audio_hw_device *dev, audio_io_handle_t handle,
                              audio_devices_t devices, audio_output_flags_t flags,
                              struct audio_config *config,
                              struct audio_stream_out **stream_out, const char *address);
    void (*close_output_stream)(struct audio_hw_device *dev,
                                struct audio_stream_out *stream_out);
    int (*open_input_stream)(struct audio_hw_device *dev, audio_io_handle_t handle,
                             audio_devices_t devices, struct audio_config *config,
                             struct audio_stream_in **stream_in, audio_input_flags_t flags,
                             const char *address, audio_source_t source);
    void (*close_input_stream)(struct audio_hw_device *dev,
                               struct audio_stream_in *stream_in);
    int (*get_microphones)(const struct audio_hw_device *dev,
                           struct audio_microphone_characteristic_t *mic_array,
                           size_t *mic_count);
    int (*dump)(const struct audio_hw_device *dev, int fd);
};
typedef struct audio_hw_device audio_hw_device_t;

#ifdef __cplusplus
}
#define AUDIO_HAL_ABI_ASSERT(test, message) static_assert(test, message)
#else
#define AUDIO_HAL_ABI_ASSERT(test, message) _Static_assert(test, message)
#endif

AUDIO_HAL_ABI_ASSERT(sizeof(void *) == 8 && sizeof(size_t) == 8 && sizeof(ssize_t) == 8,
                     "LP64 pointer/size ABI");
AUDIO_HAL_ABI_ASSERT(sizeof(bool) == 1 && sizeof(float) == 4, "HAL scalar ABI");
AUDIO_HAL_ABI_ASSERT(sizeof(hw_module_methods_t) == 8, "module methods ABI");
AUDIO_HAL_ABI_ASSERT(sizeof(hw_module_t) == 248, "complete module ABI");
AUDIO_HAL_ABI_ASSERT(offsetof(hw_module_t, methods) == 32, "module methods offset");
AUDIO_HAL_ABI_ASSERT(offsetof(hw_module_t, dso) == 40, "module dso offset");
AUDIO_HAL_ABI_ASSERT(sizeof(hw_device_t) == 120, "complete common device ABI");
AUDIO_HAL_ABI_ASSERT(offsetof(hw_device_t, close) == 112, "common close offset");
AUDIO_HAL_ABI_ASSERT(sizeof(struct audio_stream) == 112, "14 common stream callbacks");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_stream, standby) == 48, "stream standby offset");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_stream, dump) == 56, "stream dump offset");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_stream, set_parameters) == 80,
                     "stream parameters offset");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_stream_in, common) == 0, "input common prefix");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_stream_in, read) == 120, "input read offset");
AUDIO_HAL_ABI_ASSERT(sizeof(struct audio_stream_in) == 128, "input prefix through read");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_hw_device, common) == 0, "device common prefix");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_hw_device, open_input_stream) == 224,
                     "device input open offset");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_hw_device, close_input_stream) == 232,
                     "device input close offset");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_hw_device, get_microphones) == 240,
                     "microphones slot MUST precede dump");
AUDIO_HAL_ABI_ASSERT(offsetof(struct audio_hw_device, dump) == 248, "device dump offset");
AUDIO_HAL_ABI_ASSERT(sizeof(struct audio_hw_device) == 256, "device prefix through dump");
#undef AUDIO_HAL_ABI_ASSERT
#endif
