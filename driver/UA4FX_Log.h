#ifndef UA4FX_LOG_H
#define UA4FX_LOG_H
#include <os/log.h>
#include <stdio.h>
#include <stdarg.h>

static inline os_log_t ua4fx_logger(void) {
    static os_log_t l;
    if (!l) l = os_log_create("com.ua4fx.driver", "usb");
    return l;
}
static inline void ua4fx_logf(int isError, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void ua4fx_logf(int isError, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    os_log_with_type(ua4fx_logger(), isError ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT, "%{public}s", buf);
#ifdef UA4FX_LOG_STDERR
    fprintf(stderr, "[ua4fx] %s\n", buf);
#endif
}
#define LOG(...)  ua4fx_logf(0, __VA_ARGS__)
#define LOGE(...) ua4fx_logf(1, __VA_ARGS__)
#endif
