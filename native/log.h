/*
 * log.h — logcat logging for the Clarifae effect, tag "Clarifae".
 * Runtime-gated by clarifae_log_level (0=silent .. 3=verbose), which is
 * refreshed from persist.clarifae.loglevel by config.c.
 *
 * The effect runs inside audioserver, which SELinux forbids from writing to
 * /data/adb, so the .so logs ONLY to logcat. View with:  logcat -s Clarifae
 */
#ifndef CLARIFAE_LOG_H
#define CLARIFAE_LOG_H

#include <android/log.h>
#include <stdatomic.h>

#define CLARIFAE_LOG_TAG "Clarifae"

/* 0 = off, 1 = errors/warn, 2 = info, 3 = debug/verbose */
extern _Atomic int clarifae_log_level;

/* Multiple instances may refresh this shared diagnostic setting. */
#define CLARIFAE_LOG_LEVEL() atomic_load_explicit(&clarifae_log_level, memory_order_relaxed)

#define CLARIFAE_LOGE(...) do { if (CLARIFAE_LOG_LEVEL() >= 1) \
    __android_log_print(ANDROID_LOG_ERROR, CLARIFAE_LOG_TAG, __VA_ARGS__); } while (0)
#define CLARIFAE_LOGW(...) do { if (CLARIFAE_LOG_LEVEL() >= 1) \
    __android_log_print(ANDROID_LOG_WARN,  CLARIFAE_LOG_TAG, __VA_ARGS__); } while (0)
#define CLARIFAE_LOGI(...) do { if (CLARIFAE_LOG_LEVEL() >= 2) \
    __android_log_print(ANDROID_LOG_INFO,  CLARIFAE_LOG_TAG, __VA_ARGS__); } while (0)
#define CLARIFAE_LOGD(...) do { if (CLARIFAE_LOG_LEVEL() >= 3) \
    __android_log_print(ANDROID_LOG_DEBUG, CLARIFAE_LOG_TAG, __VA_ARGS__); } while (0)
#define CLARIFAE_LOGV(...) do { if (CLARIFAE_LOG_LEVEL() >= 3) \
    __android_log_print(ANDROID_LOG_VERBOSE, CLARIFAE_LOG_TAG, __VA_ARGS__); } while (0)

#endif /* CLARIFAE_LOG_H */
