/* SPDX-License-Identifier: Apache-2.0
 * In-memory stock-HAL fixture. No constructors, hardware, file or clock I/O.
 * Compile as C11, PIC/shared (Android) or dynamiclib (macOS).
 */
#include "fake_vendor.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define GUARD_WORDS 8
#define GUARD_BASE UINT64_C(0xd15ea5e0cafef000)

struct fake_device {
    struct audio_hw_device api;
    uint64_t tail_guard[GUARD_WORDS];
    struct audio_hw_device expected;
    bool used, live, mic_mute;
    unsigned open_leases;
    float master_volume;
};
struct fake_stream {
    struct audio_stream_in api;
    uint64_t tail_guard[GUARD_WORDS];
    struct audio_stream_in expected;
    bool used;
    uint32_t seed;
    struct fake_stream_state state;
};
static struct fake_device devices[FAKE_MAX_DEVICES];
static struct fake_stream streams[FAKE_MAX_STREAMS];
static struct fake_state state;
static uint32_t default_rate = FAKE_DEFAULT_RATE;
static audio_channel_mask_t default_channels = FAKE_DEFAULT_CHANNELS;
static audio_format_t default_format = FAKE_DEFAULT_FORMAT;
static uint32_t default_seed = FAKE_DEFAULT_SEED;
static int device_open_result, input_open_result, standby_result, parameters_result;
static int device_dump_result, stream_dump_result, device_close_result;
static bool force_read, force_next_read;
static ssize_t read_result, next_read_result;
static bool singleton_mode,standby_unblocks;
static pthread_mutex_t read_gate_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_gate_cond=PTHREAD_COND_INITIALIZER;
static bool read_gate_blocked;
static unsigned read_gate_entries;

static void copy_text(char out[FAKE_TEXT_CAPACITY], const char *text)
{
    size_t n = 0;
    if (text) {
        while (n + 1 < FAKE_TEXT_CAPACITY && text[n]) {
            out[n] = text[n];
            ++n;
        }
    }
    out[n] = 0;
}
static void set_guards(uint64_t guard[GUARD_WORDS])
{
    size_t i;
    for (i = 0; i < GUARD_WORDS; ++i) guard[i] = GUARD_BASE + i;
}
static bool guards_ok(const uint64_t guard[GUARD_WORDS])
{
    size_t i;
    for (i = 0; i < GUARD_WORDS; ++i)
        if (guard[i] != GUARD_BASE + i) return false;
    return true;
}
static struct fake_device *find_device(const void *pointer, bool require_live)
{
    size_t i;
    state.last_receiver = pointer;
    for (i = 0; i < FAKE_MAX_DEVICES; ++i)
        if (devices[i].used && pointer == (const void *)&devices[i].api &&
            (!require_live || devices[i].live)) return &devices[i];
    ++state.invalid_pointer_calls;
    return NULL;
}
static struct fake_stream *find_stream(const void *pointer, bool require_live)
{
    size_t i;
    state.last_receiver = pointer;
    for (i = 0; i < FAKE_MAX_STREAMS; ++i)
        if (streams[i].used && pointer == (const void *)&streams[i].api &&
            (!require_live || streams[i].state.live)) return &streams[i];
    ++state.invalid_pointer_calls;
    return NULL;
}
static char *empty_parameters(void)
{
    char *text = (char *)malloc(1);
    if (text) text[0] = 0;
    return text;
}
static size_t sample_bytes(audio_format_t format)
{
    return format == AUDIO_FORMAT_PCM_FLOAT ? 4u :
           format == AUDIO_FORMAT_PCM_16_BIT ? 2u : 1u;
}
static size_t stream_buffer_size(const struct fake_stream *stream)
{
    unsigned channels = 0;
    uint32_t bits = stream->state.channels & UINT32_C(0x3fffffff);
    while (bits) { channels += bits & 1u; bits >>= 1; }
    return (size_t)480 * channels * sample_bytes(stream->state.format);
}
static uint32_t sample_hash(uint32_t seed, uint64_t sample)
{
    uint32_t x = seed + (uint32_t)sample * UINT32_C(0x9e3779b9);
    x ^= (uint32_t)(sample >> 32);
    x ^= x >> 16; x *= UINT32_C(0x7feb352d);
    x ^= x >> 15; x *= UINT32_C(0x846ca68b);
    return x ^ (x >> 16);
}
static void fill_noise(struct fake_stream *stream, void *buffer, size_t count)
{
    unsigned char *out = (unsigned char *)buffer;
    const size_t width = sample_bytes(stream->state.format);
    size_t i;
    /* Byte-addressed cursor preserves deterministic partitioning, including
     * odd partial reads and deliberately unaligned buffers. */
    for (i = 0; i < count; ++i) {
        uint64_t offset = stream->state.byte_cursor + i;
        uint32_t hash = sample_hash(stream->seed, offset / width);
        int32_t signed_sample = (int32_t)(hash & UINT32_C(0xffff)) - 32768;
        unsigned char bytes[4] = {0, 0, 0, 0};
        if (stream->state.format == AUDIO_FORMAT_PCM_FLOAT) {
            float value = (float)signed_sample / 32768.0f;
            memcpy(bytes, &value, sizeof(value));
        } else if (stream->state.format == AUDIO_FORMAT_PCM_16_BIT) {
            int16_t value = (int16_t)signed_sample;
            memcpy(bytes, &value, sizeof(value));
        } else {
            bytes[0] = (unsigned char)hash;
        }
        out[i] = bytes[offset % width];
    }
    stream->state.byte_cursor += count;
}

static uint32_t stream_get_rate(const struct audio_stream *api)
{
    struct fake_stream *s = find_stream(api, true);
    return s ? s->state.rate : 0;
}
static int stream_set_rate(struct audio_stream *api, uint32_t rate)
{
    struct fake_stream *s = find_stream(api, true);
    if (!s) return -EINVAL;
    s->state.rate = rate;
    return 0;
}
static size_t stream_get_buffer_size(const struct audio_stream *api)
{
    struct fake_stream *s = find_stream(api, true);
    return s ? stream_buffer_size(s) : 0;
}
static audio_channel_mask_t stream_get_channels(const struct audio_stream *api)
{
    struct fake_stream *s = find_stream(api, true);
    return s ? s->state.channels : 0;
}
static audio_format_t stream_get_format(const struct audio_stream *api)
{
    struct fake_stream *s = find_stream(api, true);
    return s ? s->state.format : 0;
}
static int stream_set_format(struct audio_stream *api, audio_format_t format)
{
    struct fake_stream *s = find_stream(api, true);
    if (!s) return -EINVAL;
    s->state.format = format;
    return 0;
}
static int stream_standby(struct audio_stream *api)
{
    struct fake_stream *s = find_stream(api, true);
    ++state.standby_calls;
    if (!s) return -EINVAL;
    ++s->state.standby_calls;
    if (!standby_result) s->state.in_standby = true;
    if(standby_unblocks){pthread_mutex_lock(&read_gate_mutex);read_gate_blocked=false;pthread_cond_broadcast(&read_gate_cond);pthread_mutex_unlock(&read_gate_mutex);}
    return standby_result;
}
static int stream_dump(const struct audio_stream *api, int fd)
{
    struct fake_stream *s = find_stream(api, true);
    ++state.stream_dump_calls;
    state.last_stream_dump_fd = fd;
    if (!s) return -EINVAL;
    ++s->state.dump_calls;
    s->state.last_dump_fd = fd;
    return stream_dump_result;
}
static audio_devices_t stream_get_device(const struct audio_stream *api)
{
    struct fake_stream *s = find_stream(api, true);
    return s ? s->state.devices : 0;
}
static int stream_set_device(struct audio_stream *api, audio_devices_t value)
{
    struct fake_stream *s = find_stream(api, true);
    if (!s) return -EINVAL;
    s->state.devices = value;
    return 0;
}
static int stream_set_parameters(struct audio_stream *api, const char *parameters)
{
    struct fake_stream *s = find_stream(api, true);
    ++state.stream_parameter_calls;
    if (!s) return -EINVAL;
    ++s->state.parameter_calls;
    s->state.last_parameters = parameters;
    copy_text(s->state.parameters_copy, parameters);
    return parameters ? parameters_result : -EINVAL;
}
static char *stream_get_parameters(const struct audio_stream *api, const char *keys)
{
    (void)keys;
    return find_stream(api, true) ? empty_parameters() : NULL;
}
static int stream_add_effect(const struct audio_stream *api, effect_handle_t effect)
{
    struct fake_stream *s = find_stream(api, true);
    if (!s) return -EINVAL;
    ++s->state.add_effect_calls;
    s->state.last_effect = effect;
    state.last_added_effect = effect;
    return 0;
}
static int stream_remove_effect(const struct audio_stream *api, effect_handle_t effect)
{
    struct fake_stream *s = find_stream(api, true);
    if (!s) return -EINVAL;
    ++s->state.remove_effect_calls;
    s->state.last_effect = effect;
    state.last_removed_effect = effect;
    return 0;
}
static int stream_set_gain(struct audio_stream_in *api, float gain)
{
    (void)gain;
    return find_stream(api, true) ? 0 : -EINVAL;
}
static ssize_t stream_read(struct audio_stream_in *api, void *buffer, size_t bytes)
{
    struct fake_stream *s = find_stream(api, true);
    ssize_t result;
    size_t written = 0;
    ++state.read_calls;
    state.last_read_buffer = buffer;
    state.last_requested = bytes;
    state.last_written = 0;
    if (!s) { state.last_read_result = -EINVAL; return -EINVAL; }
    ++s->state.read_calls;
    pthread_mutex_lock(&read_gate_mutex);read_gate_entries++;pthread_cond_broadcast(&read_gate_cond);
    while(read_gate_blocked)pthread_cond_wait(&read_gate_cond,&read_gate_mutex);
    pthread_mutex_unlock(&read_gate_mutex);
    if (force_next_read) {
        result = next_read_result;
        force_next_read = false;
    } else if (force_read) {
        result = read_result;
    } else {
        result = bytes > (size_t)PTRDIFF_MAX ? -EINVAL : (ssize_t)bytes;
    }
    if (!buffer && bytes) result = -EINVAL;
    if (result > 0) {
        written = (size_t)result < bytes ? (size_t)result : bytes;
        fill_noise(s, buffer, written);
        s->state.in_standby = false;
    }
    state.last_written = s->state.last_written = written;
    state.last_read_result = s->state.last_result = result;
    s->state.last_requested = bytes;
    return result;
}
static void initialize_stream(struct fake_stream *s, struct audio_hw_device *owner,
                              audio_devices_t input_device, audio_source_t source)
{
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->seed = default_seed;
    s->state.stream = &s->api;
    s->state.owner = owner;
    s->state.live = true;
    s->state.in_standby = true;
    s->state.rate = default_rate;
    s->state.channels = default_channels;
    s->state.format = default_format;
    s->state.devices = input_device;
    s->state.source = source;
    s->api.common.get_sample_rate = stream_get_rate;
    s->api.common.set_sample_rate = stream_set_rate;
    s->api.common.get_buffer_size = stream_get_buffer_size;
    s->api.common.get_channels = stream_get_channels;
    s->api.common.get_format = stream_get_format;
    s->api.common.set_format = stream_set_format;
    s->api.common.standby = stream_standby;
    s->api.common.dump = stream_dump;
    s->api.common.get_device = stream_get_device;
    s->api.common.set_device = stream_set_device;
    s->api.common.set_parameters = stream_set_parameters;
    s->api.common.get_parameters = stream_get_parameters;
    s->api.common.add_audio_effect = stream_add_effect;
    s->api.common.remove_audio_effect = stream_remove_effect;
    s->api.set_gain = stream_set_gain;
    s->api.read = stream_read;
    s->expected = s->api;
    set_guards(s->tail_guard);
}

static uint32_t device_get_supported_devices(const struct audio_hw_device *api)
{
    (void)find_device(api, true);
    return 0; /* Deprecated since HAL 2.0; the slot still exists. */
}
static int device_init_check(const struct audio_hw_device *api)
{
    return find_device(api, true) ? 0 : -EINVAL;
}
static int device_set_voice_volume(struct audio_hw_device *api, float volume)
{
    (void)volume;
    return find_device(api, true) ? 0 : -EINVAL;
}
static int device_set_master_volume(struct audio_hw_device *api, float volume)
{
    struct fake_device *d = find_device(api, true);
    if (!d) return -EINVAL;
    d->master_volume = volume;
    return 0;
}
static int device_get_master_volume(struct audio_hw_device *api, float *volume)
{
    struct fake_device *d = find_device(api, true);
    if (!d || !volume) return -EINVAL;
    *volume = d->master_volume;
    return 0;
}
static int device_set_mode(struct audio_hw_device *api, audio_mode_t mode)
{
    (void)mode;
    return find_device(api, true) ? 0 : -EINVAL;
}
static int device_set_mic_mute(struct audio_hw_device *api, bool mute)
{
    struct fake_device *d = find_device(api, true);
    if (!d) return -EINVAL;
    d->mic_mute = mute;
    return 0;
}
static int device_get_mic_mute(const struct audio_hw_device *api, bool *mute)
{
    struct fake_device *d = find_device(api, true);
    if (!d || !mute) return -EINVAL;
    *mute = d->mic_mute;
    return 0;
}
static int device_set_parameters(struct audio_hw_device *api, const char *parameters)
{
    struct fake_device *d = find_device(api, true);
    ++state.device_parameter_calls;
    copy_text(state.device_parameters_copy, parameters);
    return !d || !parameters ? -EINVAL : parameters_result;
}
static char *device_get_parameters(const struct audio_hw_device *api, const char *keys)
{
    (void)keys;
    return find_device(api, true) ? empty_parameters() : NULL;
}
static size_t device_get_input_buffer_size(const struct audio_hw_device *api,
                                           const struct audio_config *config)
{
    (void)config; /* The fixture MUST NOT infer/dereference audio_config layout. */
    return find_device(api, true) ? (size_t)480 * sample_bytes(default_format) : 0;
}
static int device_open_output(struct audio_hw_device *api, audio_io_handle_t handle,
                              audio_devices_t output_device, audio_output_flags_t flags,
                              struct audio_config *config,
                              struct audio_stream_out **out, const char *address)
{
    (void)handle; (void)output_device; (void)flags; (void)config; (void)address;
    if (out) *out = NULL;
    return find_device(api, true) ? -ENOSYS : -EINVAL;
}
static void device_close_output(struct audio_hw_device *api, struct audio_stream_out *out)
{
    (void)out;
    (void)find_device(api, true);
}
static int device_open_input(struct audio_hw_device *api, audio_io_handle_t handle,
                             audio_devices_t input_device, struct audio_config *config,
                             struct audio_stream_in **out, audio_input_flags_t flags,
                             const char *address, audio_source_t source)
{
    size_t i;
    struct fake_device *d = find_device(api, true);
    struct fake_input_open_args *args = &state.last_input_open;
    ++state.input_open_calls;
    args->device = api; args->handle = handle; args->devices = input_device;
    args->config = config; args->stream_out = out; args->flags = flags;
    args->address = address; args->source = source;
    copy_text(args->address_copy, address);
    if (out) *out = NULL;
    if (!d || !out) return -EINVAL;
    if (input_open_result) return input_open_result;
    for (i = 0; i < FAKE_MAX_STREAMS; ++i) {
        if (!streams[i].state.live) {
            initialize_stream(&streams[i], api, input_device, source);
            *out = &streams[i].api;
            state.last_stream = *out;
            ++state.live_streams;
            return 0;
        }
    }
    return -ENOMEM;
}
static void device_close_input(struct audio_hw_device *api, struct audio_stream_in *input)
{
    struct fake_device *d = find_device(api, true);
    struct fake_stream *s = find_stream(input, true);
    ++state.input_close_calls;
    if (!d || !s) return;
    if (s->state.owner != api) { ++state.invalid_pointer_calls; return; }
    ++s->state.close_calls;
    s->state.live = false;
    --state.live_streams;
}
static int device_get_microphones(const struct audio_hw_device *api,
                                  struct audio_microphone_characteristic_t *array,
                                  size_t *count)
{
    (void)array;
    if (!find_device(api, true) || !count) return -EINVAL;
    *count = 0;
    return 0;
}
static int device_dump(const struct audio_hw_device *api, int fd)
{
    struct fake_device *d = find_device(api, true);
    ++state.device_dump_calls;
    state.last_device_dump_fd = fd;
    return d ? device_dump_result : -EINVAL;
}
static int device_close(struct hw_device_t *api)
{
    size_t i;
    struct fake_device *d = find_device(api, true);
    ++state.device_close_calls;
    if (!d) return -EINVAL;
    if (device_close_result) return device_close_result;
    if(d->open_leases>1){d->open_leases--;return 0;}
    for (i = 0; i < FAKE_MAX_STREAMS; ++i)
        if (streams[i].state.live && streams[i].state.owner == &d->api) return -EBUSY;
    d->live = false;
    --state.live_devices;
    return 0;
}
static void initialize_device(struct fake_device *d, const struct hw_module_t *module)
{
    size_t i;
    memset(d, 0, sizeof(*d));
    d->used = d->live = true;
    d->open_leases=1;
    d->master_volume = 1.0f;
    d->api.common.tag = HARDWARE_DEVICE_TAG;
    d->api.common.version = AUDIO_DEVICE_API_VERSION_3_2;
    d->api.common.module = (struct hw_module_t *)module;
    for (i = 0; i < 12; ++i) d->api.common.reserved[i] = GUARD_BASE + i;
    d->api.common.close = device_close;
    d->api.get_supported_devices = device_get_supported_devices;
    d->api.init_check = device_init_check;
    d->api.set_voice_volume = device_set_voice_volume;
    d->api.set_master_volume = device_set_master_volume;
    d->api.get_master_volume = device_get_master_volume;
    d->api.set_mode = device_set_mode;
    d->api.set_mic_mute = device_set_mic_mute;
    d->api.get_mic_mute = device_get_mic_mute;
    d->api.set_parameters = device_set_parameters;
    d->api.get_parameters = device_get_parameters;
    d->api.get_input_buffer_size = device_get_input_buffer_size;
    d->api.open_output_stream = device_open_output;
    d->api.close_output_stream = device_close_output;
    d->api.open_input_stream = device_open_input;
    d->api.close_input_stream = device_close_input;
    d->api.get_microphones = device_get_microphones;
    d->api.dump = device_dump;
    d->expected = d->api;
    set_guards(d->tail_guard);
}
static int module_open(const struct hw_module_t *module, const char *id,
                        struct hw_device_t **out)
{
    size_t i;
    ++state.device_open_calls;
    state.last_receiver = module;
    state.last_device_open.module = module;
    state.last_device_open.id = id;
    state.last_device_open.device_out = out;
    copy_text(state.last_device_open.id_copy, id);
    if (out) *out = NULL;
    if (module != &HMI.common) { ++state.invalid_pointer_calls; return -EINVAL; }
    if (!out || !id || strcmp(id, AUDIO_HARDWARE_INTERFACE)) return -EINVAL;
    if (device_open_result) return device_open_result;
    if(singleton_mode)for(i=0;i<FAKE_MAX_DEVICES;i++)if(devices[i].live){devices[i].open_leases++;*out=&devices[i].api.common;state.last_device=&devices[i].api;return 0;}
    for (i = 0; i < FAKE_MAX_DEVICES; ++i) {
        if (!devices[i].live) {
            initialize_device(&devices[i], module);
            *out = &devices[i].api.common;
            state.last_device = &devices[i].api;
            ++state.live_devices;
            return 0;
        }
    }
    return -ENOMEM;
}
static struct hw_module_methods_t module_methods = { .open = module_open };
FAKE_HMI_API struct audio_module HMI = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Deterministic fake QTI Audio HAL",
        .author = "Clarifae test fixture (no hardware)",
        .methods = &module_methods,
        .dso = NULL,
        .reserved = {0}
    }
};

const struct hw_module_t *fake_module(void) { return &HMI.common; }
const struct fake_state *fake_get_state(void) { return &state; }
int fake_get_stream_state(const struct audio_stream_in *api, struct fake_stream_state *out)
{
    struct fake_stream *s = find_stream(api, false);
    if (!s || !out) return -EINVAL;
    *out = s->state;
    return 0;
}
void fake_reset(void)
{
    memset(devices, 0, sizeof(devices));
    memset(streams, 0, sizeof(streams));
    memset(&state, 0, sizeof(state));
    default_rate = FAKE_DEFAULT_RATE;
    default_channels = FAKE_DEFAULT_CHANNELS;
    default_format = FAKE_DEFAULT_FORMAT;
    default_seed = FAKE_DEFAULT_SEED;
    state.last_input_open.source = FAKE_DEFAULT_SOURCE;
    device_open_result = input_open_result = standby_result = parameters_result = 0;
    device_dump_result = stream_dump_result = device_close_result = 0;
    force_read = force_next_read = false;
    read_result = next_read_result = 0;
    singleton_mode=false;standby_unblocks=false;
    pthread_mutex_lock(&read_gate_mutex);read_gate_blocked=false;read_gate_entries=0;pthread_mutex_unlock(&read_gate_mutex);
}
void fake_set_standby_unblocks(int enabled){standby_unblocks=enabled!=0;}
void fake_set_singleton_mode(int enabled){singleton_mode=enabled!=0;}
void fake_block_reads(int blocked){pthread_mutex_lock(&read_gate_mutex);read_gate_blocked=blocked!=0;if(blocked)read_gate_entries=0;pthread_cond_broadcast(&read_gate_cond);pthread_mutex_unlock(&read_gate_mutex);}
void fake_wait_read_entered(void){pthread_mutex_lock(&read_gate_mutex);while(!read_gate_entries)pthread_cond_wait(&read_gate_cond,&read_gate_mutex);pthread_mutex_unlock(&read_gate_mutex);}
void fake_set_open_device_result(int result) { device_open_result = result; }
void fake_set_open_input_result(int result) { input_open_result = result; }
void fake_set_config(uint32_t rate, audio_channel_mask_t channels, audio_format_t format)
{
    default_rate = rate; default_channels = channels; default_format = format;
}
int fake_set_stream_config(struct audio_stream_in *api, uint32_t rate,
                           audio_channel_mask_t channels, audio_format_t format)
{
    struct fake_stream *s = find_stream(api, true);
    if (!s) return -EINVAL;
    s->state.rate = rate; s->state.channels = channels; s->state.format = format;
    return 0;
}
void fake_set_seed(uint32_t seed) { default_seed = seed; }
int fake_rewind_stream(struct audio_stream_in *api, uint32_t seed)
{
    struct fake_stream *s = find_stream(api, true);
    if (!s) return -EINVAL;
    s->seed = seed; s->state.byte_cursor = 0;
    return 0;
}
void fake_set_read_result(ssize_t result) { force_read = true; read_result = result; }
void fake_set_next_read_result(ssize_t result)
{
    force_next_read = true; next_read_result = result;
}
void fake_set_read_default(void) { force_read = force_next_read = false; }
void fake_set_standby_result(int result) { standby_result = result; }
void fake_set_parameters_result(int result) { parameters_result = result; }
void fake_set_dump_results(int device_result, int stream_result)
{
    device_dump_result = device_result; stream_dump_result = stream_result;
}
void fake_set_close_device_result(int result) { device_close_result = result; }

int fake_check_integrity(void)
{
    size_t i;
    bool ok = true;
    for (i = 0; i < FAKE_MAX_DEVICES; ++i) {
        struct fake_device *d = &devices[i];
        if (!d->used) continue;
        /* Exclude close@112, open_input@224, close_input@232 and dump@248. */
        if (memcmp(&d->api, &d->expected, 112) ||
            memcmp((const unsigned char *)&d->api + 120,
                   (const unsigned char *)&d->expected + 120, 104) ||
            d->api.get_microphones != d->expected.get_microphones ||
            !guards_ok(d->tail_guard)) ok = false;
    }
    for (i = 0; i < FAKE_MAX_STREAMS; ++i) {
        struct fake_stream *s = &streams[i];
        if (!s->used) continue;
        /* Exclude standby@48, dump@56, set_parameters@80 and read@120. */
        if (memcmp(&s->api, &s->expected, 48) ||
            memcmp((const unsigned char *)&s->api + 64,
                   (const unsigned char *)&s->expected + 64, 16) ||
            memcmp((const unsigned char *)&s->api + 88,
                   (const unsigned char *)&s->expected + 88, 32) ||
            !guards_ok(s->tail_guard)) ok = false;
    }
    if (!ok) { ++state.integrity_failures; return -EFAULT; }
    return 0;
}

/* Optional host-only self-test; not present in normal shared-library builds.
 * LLVM IR -> lli permits execution without writing build artifacts anywhere.
 */
#ifdef FAKE_VENDOR_SELFTEST
#include <assert.h>
int main(void)
{
    const struct hw_module_t *module = fake_module();
    struct hw_module_t wrong_module = *module;
    struct hw_device_t *common = NULL;
    struct audio_hw_device *d;
    struct audio_stream_in *s = NULL, *s2 = NULL;
    struct audio_config *opaque = (struct audio_config *)(uintptr_t)0x1234;
    struct fake_stream_state ss;
    const char address[] = "fixture:original-address";
    unsigned char a[514], b[514];
    size_t i;
    effect_handle_t effect = (effect_handle_t)(uintptr_t)0x12345678;
    fake_reset();
    assert(sizeof(*module) == 248 && module == &HMI.common);
    assert(module->methods->open(&wrong_module, AUDIO_HARDWARE_INTERFACE, &common) == -EINVAL);
    assert(!common && state.invalid_pointer_calls == 1);
    fake_reset();
    fake_set_open_device_result(-ENODEV);
    assert(module->methods->open(module, AUDIO_HARDWARE_INTERFACE, &common) == -ENODEV);
    assert(!common && state.last_device_open.device_out == &common);
    fake_set_open_device_result(0);
    assert(module->methods->open(module, AUDIO_HARDWARE_INTERFACE, &common) == 0);
    d = (struct audio_hw_device *)common;
    assert(d == state.last_device && common->module == module);
    assert(d->init_check(d) == 0);
    fake_set_open_input_result(-EBUSY);
    assert(d->open_input_stream(d, 51, 0x80000004u, opaque, &s, 0x12, address, 1) == -EBUSY);
    assert(!s && state.last_input_open.config == opaque);
    fake_set_open_input_result(0);
    assert(d->open_input_stream(d, 51, 0x80000004u, opaque, &s, 0x12, address, 1) == 0);
    assert(s == state.last_stream && state.last_input_open.stream_out == &s);
    assert(state.last_input_open.device == d && state.last_input_open.address == address);
    assert(state.last_input_open.handle == 51 && state.last_input_open.devices == 0x80000004u);
    assert(state.last_input_open.flags == 0x12 && state.last_input_open.source == 1);
    assert(s->common.get_sample_rate(&s->common) == 48000);
    assert(s->common.get_channels(&s->common) == AUDIO_CHANNEL_IN_MONO);
    assert(s->common.get_format(&s->common) == AUDIO_FORMAT_PCM_16_BIT);
    assert(s->common.get_buffer_size(&s->common) == 960);
    memset(a, 0xa5, sizeof(a));
    assert(s->read(s, a + 1, 257) == 257 && a[0] == 0xa5 && a[258] == 0xa5);
    assert(fake_rewind_stream(s, FAKE_DEFAULT_SEED) == 0);
    memset(b, 0xa5, sizeof(b));
    assert(s->read(s, b + 1, 63) == 63 && s->read(s, b + 64, 194) == 194);
    assert(!memcmp(a, b, sizeof(a)));
    fake_set_next_read_result(7);
    memset(b, 0xa5, sizeof(b));
    assert(s->read(s, b + 1, 64) == 7 && b[0] == 0xa5 && b[8] == 0xa5);
    assert(state.last_written == 7 && s->read(s, b + 1, 64) == 64);
    fake_set_read_result(-EIO);
    memset(b, 0xa5, sizeof(b));
    assert(s->read(s, b + 1, 64) == -EIO && state.last_written == 0);
    for (i = 0; i < sizeof(b); ++i) assert(b[i] == 0xa5);
    fake_set_next_read_result(0);
    assert(s->read(s, b + 1, 64) == 0 && s->read(s, b + 1, 64) == -EIO);
    fake_set_next_read_result(1000);
    assert(s->read(s, b + 1, 64) == 1000 && state.last_written == 64);
    assert(b[0] == 0xa5 && b[65] == 0xa5);
    fake_set_read_default();
    assert(fake_set_stream_config(s, 44100, AUDIO_CHANNEL_IN_STEREO, AUDIO_FORMAT_PCM_FLOAT) == 0);
    assert(s->common.get_sample_rate(&s->common) == 44100);
    assert(s->common.get_channels(&s->common) == AUDIO_CHANNEL_IN_STEREO);
    assert(s->common.get_format(&s->common) == AUDIO_FORMAT_PCM_FLOAT);
    assert(fake_rewind_stream(s, FAKE_DEFAULT_SEED) == 0);
    assert(s->read(s, a, 512) == 512);
    for (i = 0; i < 128; ++i) {
        float value;
        memcpy(&value, a + i * 4, 4);
        assert(value >= -1.0f && value < 1.0f);
    }
    fake_set_config(16000, AUDIO_CHANNEL_IN_STEREO, AUDIO_FORMAT_PCM_16_BIT);
    assert(d->open_input_stream(d, 52, 0x80000004u, opaque, &s2, 0, address, 7) == 0);
    assert(fake_get_stream_state(s2, &ss) == 0 && ss.rate == 16000 && ss.source == 7);
    assert(s->common.get_sample_rate(&s->common) == 44100);
    assert(s->common.add_audio_effect(&s->common, effect) == 0);
    assert(s->common.remove_audio_effect(&s->common, effect) == 0);
    assert(state.last_added_effect == effect && state.last_removed_effect == effect);
    assert(s->common.standby(&s->common) == 0);
    fake_set_standby_result(-EAGAIN);
    assert(s->common.standby(&s->common) == -EAGAIN);
    fake_set_dump_results(17, 23);
    assert(d->dump(d, -1) == 17 && s->common.dump(&s->common, -2) == 23);
    assert(state.last_device_dump_fd == -1 && state.last_stream_dump_fd == -2);
    fake_set_parameters_result(-ENOSYS);
    assert(d->set_parameters(d, "device=value") == -ENOSYS);
    assert(s->common.set_parameters(&s->common, address) == -ENOSYS);
    assert(fake_get_stream_state(s, &ss) == 0 && ss.last_parameters == address);
    assert(!strcmp(ss.parameters_copy, address));
    assert(fake_check_integrity() == 0);
    {
        struct audio_stream_in original = *s;
        struct audio_hw_device original_device = *d;
        s->common.standby = NULL; s->common.dump = NULL;
        s->common.set_parameters = NULL; s->read = NULL;
        d->common.close = NULL; d->open_input_stream = NULL;
        d->close_input_stream = NULL; d->dump = NULL;
        assert(fake_check_integrity() == 0);
        s->common.get_channels = NULL;
        assert(fake_check_integrity() == -EFAULT);
        *s = original; *d = original_device;
        streams[0].tail_guard[0] ^= 1;
        assert(fake_check_integrity() == -EFAULT);
        streams[0].tail_guard[0] ^= 1;
        assert(fake_check_integrity() == 0);
        assert(s->read(&original, b, 64) == -EINVAL);
    }
    assert(common->close(common) == -EBUSY);
    d->close_input_stream(d, s);
    d->close_input_stream(d, s2);
    assert(state.live_streams == 0);
    assert(fake_get_stream_state(s, &ss) == 0 && !ss.live && ss.close_calls == 1);
    fake_set_close_device_result(-EAGAIN);
    assert(common->close(common) == -EAGAIN && state.live_devices == 1);
    fake_set_close_device_result(0);
    assert(common->close(common) == 0 && state.live_devices == 0);
    assert(fake_check_integrity() == 0);
    fake_reset();
    assert(state.device_open_calls == 0 && state.invalid_pointer_calls == 0);
    return 0;
}
#endif
