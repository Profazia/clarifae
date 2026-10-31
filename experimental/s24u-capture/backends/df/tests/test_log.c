#include <stdatomic.h>
_Atomic int clarifae_log_level=0;
#ifndef __ANDROID__
int __android_log_print(int priority,const char *tag,const char *fmt,...) { (void)priority;(void)tag;(void)fmt;return 0; }
#endif
