#include "esp_log.h"

#include <stdarg.h>
#include <stdio.h>

// 宿主测试用的 ESP_LOG* 落点：直接打到 stdout。
void stub_esp_log(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("      [%s] ", tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}
