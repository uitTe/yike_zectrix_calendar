// 排班缓存的 NVS 存取。缓存是断网兜底的唯一数据来源（docs/03 §2.4）。
#include <cstring>

#include "cal_sched_net.h"
#include "esp_log.h"
#include "nvs.h"

namespace {

constexpr char kTag[] = "cal_sched";
// NVS 的命名空间与键名上限都是 15 个字符。
constexpr char kCacheNamespace[] = "cal_sched";
constexpr char kKeyDays[] = "days";

}  // namespace

bool CalSchedCacheLoad(CalSchedCache* out) {
    if (out == nullptr) {
        return false;
    }
    cal_sched_cache_clear(out);
    nvs_handle_t handle = 0;
    const esp_err_t open_err = nvs_open(kCacheNamespace, NVS_READONLY, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "尚无排班缓存");
        return false;
    }
    ESP_ERROR_CHECK(open_err);

    uint8_t blob[CAL_SCHED_CACHE_BYTES] = {};
    size_t length = sizeof(blob);
    const esp_err_t err = nvs_get_blob(handle, kKeyDays, blob, &length);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "读取排班缓存失败：%s", esp_err_to_name(err));
        return false;
    }
    if (!cal_sched_cache_unpack(blob, length, out)) {
        ESP_LOGE(kTag, "排班缓存格式不符（长度 %u），按无缓存处理",
                 static_cast<unsigned>(length));
        cal_sched_cache_clear(out);
        return false;
    }
    ESP_LOGI(kTag, "已读取排班缓存 %d 天", out->count);
    return true;
}

bool CalSchedCacheSave(const CalSchedCache& cache) {
    uint8_t blob[CAL_SCHED_CACHE_BYTES] = {};
    const size_t length = cal_sched_cache_pack(&cache, blob, sizeof(blob));
    if (length == 0) {
        ESP_LOGE(kTag, "排班缓存序列化失败");
        return false;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kCacheNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "打开缓存命名空间失败：%s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, kKeyDays, blob, length);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "写入排班缓存失败：%s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "排班缓存已写入 %d 天，%u 字节", cache.count,
             static_cast<unsigned>(length));
    return true;
}
