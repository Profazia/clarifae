/*
 * audio_effect.h — trimmed, AOSP-compatible Android audio effect ABI.
 *
 * The NDK does NOT ship hardware/audio_effect.h, so this header provides a
 * minimal, binary-compatible subset of the AOSP definitions that the Clarifae
 * pre-processing effect needs. Layouts/values match
 *   system/media/audio/include/system/audio_effect.h
 *   hardware/libhardware/include/hardware/audio_effect.h
 * (Apache-2.0, The Android Open Source Project). Keep field order/sizes EXACT —
 * AudioFlinger casts straight onto these structs.
 */
#ifndef CLARIFAE_AUDIO_EFFECT_H
#define CLARIFAE_AUDIO_EFFECT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- API versioning ----- */
#define EFFECT_MAKE_API_VERSION(M, m)  ((((M) & 0xffff) << 16) | ((m) & 0xffff))
#define EFFECT_API_VERSION_MAJOR(v)    ((v) >> 16)
#define EFFECT_API_VERSION_MINOR(v)    ((v) & 0xffff)

/* Legacy engine control ABI 2.0; library ABI is independently 3.0. */
#define EFFECT_CONTROL_API_VERSION     EFFECT_MAKE_API_VERSION(2, 0)  /* 0x00020000 */
#define EFFECT_LIBRARY_API_VERSION     EFFECT_MAKE_API_VERSION(3, 0)  /* 0x00030000 */

#define EFFECT_STRING_LEN 64

/* ----- UUID ----- */
typedef struct effect_uuid_s {
    uint32_t timeLow;
    uint16_t timeMid;
    uint16_t timeHiAndVersion;
    uint16_t clockSeq;
    uint8_t  node[6];
} effect_uuid_t;

#define EFFECT_UUID_INITIALIZER { 0xec7178ec, 0xe5e1, 0x4432, 0xa3f4, { 0x46, 0x57, 0xe6, 0x79, 0x52, 0x10 } }

/* ----- Descriptor ----- */
typedef struct effect_descriptor_s {
    effect_uuid_t type;        /* effect type (e.g. noise suppression) */
    effect_uuid_t uuid;        /* unique implementation UUID            */
    uint32_t      apiVersion;  /* EFFECT_CONTROL_API_VERSION            */
    uint32_t      flags;       /* EFFECT_FLAG_*                         */
    uint16_t      cpuLoad;     /* MIPS (0.1 MIPS units)                 */
    uint16_t      memoryUsage; /* KB                                    */
    char          name[EFFECT_STRING_LEN];
    char          implementor[EFFECT_STRING_LEN];
} effect_descriptor_t;

/* ----- Effect flags (subset we use) ----- */
#define EFFECT_FLAG_TYPE_SHIFT        0
#define EFFECT_FLAG_TYPE_PRE_PROC     (3 << EFFECT_FLAG_TYPE_SHIFT)  /* pre-processing effect */
#define EFFECT_FLAG_TYPE_POST_PROC    (4 << EFFECT_FLAG_TYPE_SHIFT)

#define EFFECT_FLAG_INSERT_SHIFT      3
#define EFFECT_FLAG_INSERT_ANY        (0 << EFFECT_FLAG_INSERT_SHIFT)
#define EFFECT_FLAG_INSERT_FIRST      (1 << EFFECT_FLAG_INSERT_SHIFT)
#define EFFECT_FLAG_INSERT_LAST       (2 << EFFECT_FLAG_INSERT_SHIFT)

#define EFFECT_FLAG_VOLUME_NONE       (0 << 6)
#define EFFECT_FLAG_DEVICE_NONE       (0 << 9)
#define EFFECT_FLAG_INPUT_DIRECT      (1 << 12)
#define EFFECT_FLAG_OUTPUT_DIRECT     (1 << 14)
#define EFFECT_FLAG_HW_ACC_NONE       0

/* ----- Audio formats (audio_format_t, truncated to the 8-bit field) ----- */
#define AUDIO_FORMAT_PCM_16_BIT       0x1u
#define AUDIO_FORMAT_PCM_8_BIT        0x2u
#define AUDIO_FORMAT_PCM_32_BIT       0x3u
#define AUDIO_FORMAT_PCM_8_24_BIT     0x4u
#define AUDIO_FORMAT_PCM_FLOAT        0x5u
#define AUDIO_FORMAT_PCM_24_BIT_PACKED 0x6u

/* ----- audio_buffer_t ----- */
typedef struct audio_buffer_s {
    size_t frameCount;   /* number of frames in buffer */
    union {
        void*    raw;
        int32_t* s32;
        int16_t* s16;
        uint8_t* u8;
        float*   f32;
    };
} audio_buffer_t;

/* ----- buffer provider callback ----- */
typedef int32_t (*buffer_function_t)(void* cookie, audio_buffer_t* buffer);

typedef struct {
    buffer_function_t getBuffer;
    buffer_function_t releaseBuffer;
    void*             cookie;
} buffer_provider_t;

/* ----- buffer_config_t / effect_config_t ----- */
typedef struct buffer_config_s {
    audio_buffer_t    buffer;       /* buffer for in-place process */
    uint32_t          samplingRate; /* sampling rate in Hz */
    uint32_t          channels;     /* channel mask (audio_channel_mask_t) */
    buffer_provider_t bufferProvider;
    uint8_t           format;       /* audio_format_t (low 8 bits) */
    uint8_t           accessMode;   /* effect_buffer_access_e */
    uint16_t          mask;         /* EFFECT_CONFIG_* : which fields are valid */
} buffer_config_t;

typedef struct effect_config_s {
    buffer_config_t inputCfg;
    buffer_config_t outputCfg;
} effect_config_t;

/* config mask bits */
#define EFFECT_CONFIG_BUFFER   0x0001
#define EFFECT_CONFIG_SMP_RATE 0x0002
#define EFFECT_CONFIG_CHANNELS 0x0004
#define EFFECT_CONFIG_FORMAT   0x0008
#define EFFECT_CONFIG_ACC_MODE 0x0010
#define EFFECT_CONFIG_ALL      (EFFECT_CONFIG_BUFFER | EFFECT_CONFIG_SMP_RATE | \
                                EFFECT_CONFIG_CHANNELS | EFFECT_CONFIG_FORMAT | \
                                EFFECT_CONFIG_ACC_MODE)

/* buffer access modes */
typedef enum {
    EFFECT_BUFFER_ACCESS_WRITE      = 0,
    EFFECT_BUFFER_ACCESS_READ       = 1,
    EFFECT_BUFFER_ACCESS_ACCUMULATE = 2
} effect_buffer_access_e;

/* simple channel masks (mono/stereo are all we special-case) */
#define AUDIO_CHANNEL_IN_MONO   0x10u
#define AUDIO_CHANNEL_IN_STEREO 0x0Cu
#define AUDIO_CHANNEL_OUT_MONO  0x1u
#define AUDIO_CHANNEL_OUT_STEREO 0x3u

/* ----- command codes (effect_command_e) ----- */
enum effect_command_e {
    EFFECT_CMD_INIT                       = 0,
    EFFECT_CMD_SET_CONFIG                 = 1,
    EFFECT_CMD_RESET                      = 2,
    EFFECT_CMD_ENABLE                     = 3,
    EFFECT_CMD_DISABLE                    = 4,
    EFFECT_CMD_SET_PARAM                  = 5,
    EFFECT_CMD_SET_PARAM_DEFERRED         = 6,
    EFFECT_CMD_SET_PARAM_COMMIT           = 7,
    EFFECT_CMD_GET_PARAM                  = 8,
    EFFECT_CMD_SET_DEVICE                 = 9,
    EFFECT_CMD_SET_VOLUME                 = 10,
    EFFECT_CMD_SET_AUDIO_MODE             = 11,
    EFFECT_CMD_SET_CONFIG_REVERSE         = 12,
    EFFECT_CMD_SET_INPUT_DEVICE           = 13,
    EFFECT_CMD_GET_CONFIG                 = 14,
    EFFECT_CMD_GET_CONFIG_REVERSE         = 15,
    EFFECT_CMD_GET_FEATURE_SUPPORTED_CONFIGS = 16,
    EFFECT_CMD_GET_FEATURE_CONFIG         = 17,
    EFFECT_CMD_SET_FEATURE_CONFIG         = 18,
    EFFECT_CMD_SET_AUDIO_SOURCE           = 19,
    EFFECT_CMD_OFFLOAD                    = 20,
    EFFECT_CMD_DUMP                       = 21,
    EFFECT_CMD_FIRST_PROPRIETARY          = 0x10000
};

/* ----- effect control interface ----- */
typedef struct effect_interface_s** effect_handle_t;

struct effect_interface_s {
    int32_t (*process)(effect_handle_t self,
                       audio_buffer_t* inBuffer,
                       audio_buffer_t* outBuffer);
    int32_t (*command)(effect_handle_t self,
                       uint32_t cmdCode, uint32_t cmdSize, void* pCmdData,
                       uint32_t* replySize, void* pReplyData);
    int32_t (*get_descriptor)(effect_handle_t self,
                              effect_descriptor_t* pDescriptor);
    int32_t (*process_reverse)(effect_handle_t self,
                               audio_buffer_t* inBuffer,
                               audio_buffer_t* outBuffer);
};
typedef struct effect_interface_s effect_interface_t;

/* ----- effect library interface ----- */
#define AUDIO_EFFECT_LIBRARY_TAG ((('A') << 24) | (('E') << 16) | (('L') << 8) | ('T')) /* 0x41454c54 */
#define AUDIO_EFFECT_LIBRARY_INFO_SYM        AELI
#define AUDIO_EFFECT_LIBRARY_INFO_SYM_AS_STR "AELI"

typedef struct audio_effect_library_s {
    uint32_t    tag;       /* AUDIO_EFFECT_LIBRARY_TAG */
    uint32_t    version;   /* EFFECT_LIBRARY_API_VERSION */
    const char* name;
    const char* implementor;
    int32_t (*create_effect)(const effect_uuid_t* uuid,
                             int32_t sessionId, int32_t ioId,
                             effect_handle_t* pHandle);
    int32_t (*release_effect)(effect_handle_t handle);
    int32_t (*get_descriptor)(const effect_uuid_t* uuid,
                              effect_descriptor_t* pDescriptor);
} audio_effect_library_t;

/* AOSP effect_param_t: value begins at the next 32-bit boundary. */
typedef struct effect_param_s {
    int32_t status;
    uint32_t psize;
    uint32_t vsize;
    unsigned char data[];
} effect_param_t;

/* Lock the 64-bit legacy 3.0 prefix and control layouts to AOSP. */
#if defined(__LP64__) && !defined(__cplusplus)
_Static_assert(sizeof(effect_uuid_t) == 16, "UUID ABI");
_Static_assert(sizeof(effect_descriptor_t) == 172, "descriptor ABI");
_Static_assert(offsetof(effect_descriptor_t, name) == 44, "descriptor name ABI");
_Static_assert(sizeof(audio_buffer_t) == 16, "audio buffer ABI");
_Static_assert(sizeof(buffer_provider_t) == 24, "provider ABI");
_Static_assert(sizeof(buffer_config_t) == 56, "buffer config ABI");
_Static_assert(offsetof(buffer_config_t, format) == 48, "format ABI");
_Static_assert(sizeof(effect_config_t) == 112, "effect config ABI");
_Static_assert(sizeof(effect_interface_t) == 32, "callback ABI");
_Static_assert(sizeof(audio_effect_library_t) == 48, "library 3.0 prefix ABI");
_Static_assert(offsetof(audio_effect_library_t, create_effect) == 24, "create ABI");
_Static_assert(sizeof(effect_param_t) == 12, "parameter ABI");
_Static_assert(AUDIO_EFFECT_LIBRARY_TAG == 0x41454c54, "library tag ABI");
_Static_assert(EFFECT_CONTROL_API_VERSION == 0x20000, "control version ABI");
_Static_assert(EFFECT_LIBRARY_API_VERSION == 0x30000, "library version ABI");
#endif

/* common return codes (errno-style negatives are also used) */
#define EFFECT_SUCCESS 0

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CLARIFAE_AUDIO_EFFECT_H */
