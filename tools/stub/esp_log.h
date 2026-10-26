// 宿主编译用的 esp_log.h 替身。
//
// app/components/cal_font/cal_font.cc 是纯逻辑（不碰 IDF 外设），只依赖 esp_log。
// 为了能在 PC 上用 g++ 编一遍、直接验证「缺字跳过」这类分支，这里提供一个最小替身：
// 把 ESP_LOG* 接到 stdout。**只有 tools/ 下的宿主测试会用到它**，固件构建永远走
// 真正的 IDF 头（`-I tools/stub` 不在 app/ 的 include 路径里）。

#ifndef TOOLS_STUB_ESP_LOG_H
#define TOOLS_STUB_ESP_LOG_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

void stub_esp_log(const char* tag, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#define ESP_LOGE(tag, fmt, ...) stub_esp_log(tag, "E " fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) stub_esp_log(tag, "W " fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) stub_esp_log(tag, "I " fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) stub_esp_log(tag, "D " fmt, ##__VA_ARGS__)

#endif  // TOOLS_STUB_ESP_LOG_H
