// 外部接口凭据（AGC Client ID / Client Secret、云函数 apiKey、分组编号）的 NVS 存取。
//
// 放在 cal_net 里而不是取数组件里：配网流程需要写它们，而配网属于本组件 ——
// 让取数组件反向依赖配网会形成组件循环。
#include <cstring>

#include "cal_net.h"
#include "esp_log.h"
#include "nvs.h"

namespace {

constexpr char kTag[] = "cal_cred";
// NVS 的命名空间与键名上限都是 15 个字符。
constexpr char kNamespace[] = "cal_agc";
constexpr char kKeyClientId[] = "clientid";
constexpr char kKeySecret[] = "secret";
constexpr char kKeyApiKey[] = "apikey";
constexpr char kKeyGroupId[] = "groupid";
constexpr char kKeyToken[] = "token";
constexpr char kKeyExpire[] = "expire";

// 读一个字符串键。键不存在算「不存在」返回 false；
// 其他错误（分区损坏、长度不符）必须暴露出来，不能当成「没配过」。
bool ReadString(nvs_handle_t handle, const char* key, char* out, size_t out_size) {
    size_t length = out_size;
    const esp_err_t err = nvs_get_str(handle, key, out, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return false;
    }
    ESP_ERROR_CHECK(err);
    return out[0] != '\0';
}

}  // namespace

bool CalApiCredLoad(CalApiCredentials* out) {
    if (out == nullptr) {
        return false;
    }
    nvs_handle_t handle = 0;
    const esp_err_t open_err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "尚未写入取数凭证：命名空间不存在");
        *out = CalApiCredentials{};
        return false;
    }
    ESP_ERROR_CHECK(open_err);

    CalApiCredentials cred;
    const bool has_id =
        ReadString(handle, kKeyClientId, cred.client_id, sizeof(cred.client_id));
    const bool has_secret =
        ReadString(handle, kKeySecret, cred.client_secret, sizeof(cred.client_secret));
    const bool has_key = ReadString(handle, kKeyApiKey, cred.api_key, sizeof(cred.api_key));
    const bool has_group =
        ReadString(handle, kKeyGroupId, cred.group_id, sizeof(cred.group_id));
    nvs_close(handle);

    *out = cred;
    if (!has_id || !has_secret || !has_key || !has_group) {
        // 只报「缺哪几项」，不打印任何值 —— 屏幕日志会被截图发出去。
        ESP_LOGW(kTag, "取数凭证不完整：clientId=%d secret=%d apiKey=%d groupId=%d", has_id,
                 has_secret, has_key, has_group);
        return false;
    }
    ESP_LOGI(kTag, "已读取取数凭证 clientId=%s groupId=%s", cred.client_id, cred.group_id);
    return true;
}

esp_err_t CalApiCredMerge(const CalApiCredentials& patch, bool* complete) {
    CalApiCredentials merged;
    CalApiCredLoad(&merged);

    if (patch.client_id[0] != '\0') {
        if (std::strlen(patch.client_id) > kCalApiClientIdMax) {
            return ESP_ERR_INVALID_SIZE;
        }
        std::strcpy(merged.client_id, patch.client_id);
    }
    if (patch.client_secret[0] != '\0') {
        if (std::strlen(patch.client_secret) > kCalApiClientSecretMax) {
            return ESP_ERR_INVALID_SIZE;
        }
        std::strcpy(merged.client_secret, patch.client_secret);
    }
    if (patch.api_key[0] != '\0') {
        if (std::strlen(patch.api_key) > kCalApiApiKeyMax) {
            return ESP_ERR_INVALID_SIZE;
        }
        std::strcpy(merged.api_key, patch.api_key);
    }
    if (patch.group_id[0] != '\0') {
        if (std::strlen(patch.group_id) > kCalApiGroupIdMax) {
            return ESP_ERR_INVALID_SIZE;
        }
        std::strcpy(merged.group_id, patch.group_id);
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "打开凭证命名空间失败：%s", esp_err_to_name(err));
        return err;
    }
    // 未填的项直接跳过，等价于保持原值。
    if (merged.client_id[0] != '\0') {
        err = nvs_set_str(handle, kKeyClientId, merged.client_id);
    }
    if (err == ESP_OK && merged.client_secret[0] != '\0') {
        err = nvs_set_str(handle, kKeySecret, merged.client_secret);
    }
    if (err == ESP_OK && merged.api_key[0] != '\0') {
        err = nvs_set_str(handle, kKeyApiKey, merged.api_key);
    }
    if (err == ESP_OK && merged.group_id[0] != '\0') {
        err = nvs_set_str(handle, kKeyGroupId, merged.group_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "写入取数凭证失败：%s", esp_err_to_name(err));
        return err;
    }

    CalApiCredentials readback;
    const bool full = CalApiCredLoad(&readback);
    if (full && (std::strcmp(readback.client_id, merged.client_id) != 0 ||
                 std::strcmp(readback.client_secret, merged.client_secret) != 0 ||
                 std::strcmp(readback.api_key, merged.api_key) != 0 ||
                 std::strcmp(readback.group_id, merged.group_id) != 0)) {
        ESP_LOGE(kTag, "取数凭证回读比对不一致");
        return ESP_ERR_INVALID_STATE;
    }
    if (complete != nullptr) {
        *complete = full;
    }
    ESP_LOGI(kTag, "取数凭证已写入并校验，是否齐全：%s", full ? "是" : "否");
    // 凭证变了，按旧凭证换来的令牌立刻作废。
    CalApiTokenClear();
    return ESP_OK;
}

esp_err_t CalApiCredErase() {
    nvs_handle_t handle = 0;
    const esp_err_t open_err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (open_err != ESP_OK) {
        return open_err;
    }
    esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGW(kTag, "取数凭证已清除：%s", esp_err_to_name(err));
    return err;
}

bool CalApiTokenLoad(char* token, size_t token_size, int64_t* expire_epoch) {
    if (token == nullptr || token_size == 0 || expire_epoch == nullptr) {
        return false;
    }
    token[0] = '\0';
    *expire_epoch = 0;
    nvs_handle_t handle = 0;
    const esp_err_t open_err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    ESP_ERROR_CHECK(open_err);

    size_t length = token_size;
    const esp_err_t token_err = nvs_get_str(handle, kKeyToken, token, &length);
    int64_t expire = 0;
    const esp_err_t expire_err = nvs_get_i64(handle, kKeyExpire, &expire);
    nvs_close(handle);

    if (token_err == ESP_ERR_NVS_NOT_FOUND && expire_err == ESP_ERR_NVS_NOT_FOUND) {
        // 从没换过令牌 —— 正常状态，不当故障。
        return false;
    }
    if (token_err != ESP_OK || expire_err != ESP_OK) {
        ESP_LOGW(kTag, "令牌缓存不完整：token=%s expire=%s", esp_err_to_name(token_err),
                 esp_err_to_name(expire_err));
        return false;
    }
    if (token[0] == '\0' || expire <= 0) {
        return false;
    }
    *expire_epoch = expire;
    return true;
}

esp_err_t CalApiTokenSave(const char* token, int64_t expire_epoch) {
    if (token == nullptr || token[0] == '\0' || expire_epoch <= 0 ||
        std::strlen(token) > kCalApiTokenMax) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, kKeyToken, token);
    if (err == ESP_OK) {
        err = nvs_set_i64(handle, kKeyExpire, expire_epoch);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "令牌写入失败：%s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(kTag, "令牌已缓存，过期时刻 %lld（本地时间）",
             static_cast<long long>(expire_epoch));
    return ESP_OK;
}

esp_err_t CalApiTokenClear() {
    nvs_handle_t handle = 0;
    const esp_err_t open_err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (open_err != ESP_OK) {
        return open_err;
    }
    // 键不存在也算清空成功 —— 调用方只关心「之后没有缓存令牌」。
    esp_err_t err = nvs_erase_key(handle, kKeyToken);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        const esp_err_t expire_err = nvs_erase_key(handle, kKeyExpire);
        if (expire_err != ESP_OK && expire_err != ESP_ERR_NVS_NOT_FOUND) {
            err = expire_err;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "令牌缓存已清除");
    }
    return err;
}
