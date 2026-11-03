#ifndef CLF_HOST_LOG_H
#define CLF_HOST_LOG_H
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_VERBOSE 2
static inline int __android_log_print(int level,const char*tag,const char*fmt,...) {(void)level;(void)tag;(void)fmt;return 0;}
#endif
