/* Minimal stand-in for <android/log.h> so nvn.c builds on a desktop host. */
#pragma once
#include <stdarg.h>
#include <stdio.h>
enum { ANDROID_LOG_INFO = 4, ANDROID_LOG_ERROR = 6 };
static inline int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prio == ANDROID_LOG_ERROR ? "[E]" : "[I]");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}
