// WiFi 凭据存取与 STA 链路连接。
#include "cal_net.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {

constexpr char kTag[] = "cal_wifi";
// NVS 的命名空间与键名上限都是 15 个字符。
constexpr char kNamespace[] = "cal_net";
constexpr char kKeySsid[] = "ssid";
constexpr char kKeyPass[] = "pass";

// 每次唤醒都是冷启动，射频与事件组随之重建，因此这些状态只需要在本次运行内有效。
EventGroupHandle_t g_events = nullptr;
esp_netif_t* g_netif = nullptr;
esp_netif_t* g_ap_netif = nullptr;
bool g_inited = false;
bool g_started = false;

constexpr EventBits_t kBitStarted = BIT0;
constexpr EventBits_t kBitConnected = BIT1;
constexpr EventBits_t kBitDisconnected = BIT2;

// 关联失败后的重试间隔。总时长仍受调用方给的超时约束。
constexpr uint32_t kBackoffMs[] = {0, 1000, 3000};

// 最近一次断开原因。关联失败时打出来，否则只能靠猜。
int g_last_disconnect_reason = 0;

const char* DisconnectReasonText(int reason) {
    switch (reason) {
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            return "四次握手超时，通常表示密码错误";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "握手超时，密码或加密方式不匹配";
        case WIFI_REASON_NO_AP_FOUND:
            return "扫描不到该网络，可能只支持 5 GHz 或信号太弱";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
            return "认证失败，密码错误或被 AP 拒绝";
        case WIFI_REASON_ASSOC_FAIL:
            return "关联失败，AP 拒绝接入";
        case WIFI_REASON_CONNECTION_FAIL:
            return "连接失败，AP 无响应";
        case WIFI_REASON_ASSOC_LEAVE:
            return "AP 主动断开";
        case WIFI_REASON_CIPHER_SUITE_REJECTED:
            return "加密套件被拒绝，检查 AP 是否只允许 WPA3";
        case 0:
            return "尚未收到断开事件，可能是超时前一直没连上";
        default:
            return "见 esp_wifi_types.h 的 wifi_err_reason_t";
    }
}

void WifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    (void)data;
    if (g_events == nullptr) {
        return;
    }
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            xEventGroupSetBits(g_events, kBitStarted);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
            g_last_disconnect_reason = event != nullptr ? event->reason : 0;
            xEventGroupSetBits(g_events, kBitDisconnected);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_events, kBitConnected);
    }
}

// 初始化射频并进入 STA 模式。事件处理器必须在 esp_wifi_start 之前注册，
// 否则 STA_START 可能先于注册到达，连接流程会一直等到超时。
bool EnsureRadio() {
    if (g_started) {
        return true;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "esp_netif_init 失败：%s", esp_err_to_name(err));
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "创建默认事件循环失败：%s", esp_err_to_name(err));
        return false;
    }
    if (g_netif == nullptr) {
        g_netif = esp_netif_create_default_wifi_sta();
        if (g_netif == nullptr) {
            ESP_LOGE(kTag, "创建 STA 网络接口失败");
            return false;
        }
    }
    if (g_events == nullptr) {
        g_events = xEventGroupCreate();
        if (g_events == nullptr) {
            ESP_LOGE(kTag, "创建事件组失败");
            return false;
        }
    }

    if (!g_inited) {
        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init_config);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "esp_wifi_init 失败：%s", esp_err_to_name(err));
            return false;
        }
        g_inited = true;
        // 凭据只在 NVS 的 cal_net 命名空间保存一份。默认的 WIFI_STORAGE_FLASH 会让
        // 驱动另写一份到 nvs.net80211，两处内容不一致时排查成本很高。
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &WifiEventHandler, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &WifiEventHandler, nullptr));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    xEventGroupClearBits(g_events, kBitStarted | kBitConnected | kBitDisconnected);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_start 失败：%s", esp_err_to_name(err));
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        g_events, kBitStarted, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    if ((bits & kBitStarted) == 0) {
        ESP_LOGE(kTag, "等待 STA_START 超时");
        return false;
    }
    g_started = true;
    return true;
}

void FillLinkInfo(int64_t began_us, CalWifiLink* link) {
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        link->rssi = ap.rssi;
    }
    esp_netif_ip_info_t ip = {};
    if (g_netif != nullptr && esp_netif_get_ip_info(g_netif, &ip) == ESP_OK) {
        esp_ip4addr_ntoa(&ip.ip, link->ip, sizeof(link->ip));
    }
    link->elapsed_ms = static_cast<int>((esp_timer_get_time() - began_us) / 1000);
}

// 写入 STA 配置，遇到 ESP_ERR_WIFI_STATE 时先把状态机拉回 idle 再重试。
//
// 上一轮关联还没收尾（sta state 不是 idle / connected）时，ESP-IDF 会直接拒绝
// esp_wifi_set_config。取数重试是「关联失败 → 立即重来」的节奏，正好撞上这个窗口：
// 2026-10-26 实测 3 轮重试只用掉 10 秒，后两轮连一帧都没发出去，全是
// `wifi:sta is connecting, cannot set config`。这里主动 disconnect 并小步重试，
// 让每次重试都有真正尝试的机会。
// 参数用非 const 引用：esp_wifi_set_config 的形参本身就不带 const。
esp_err_t ApplyStaConfig(wifi_config_t& config, uint32_t wait_ms) {
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(wait_ms) * 1000;
    esp_err_t err = ESP_OK;
    for (int attempt = 0; attempt < 10; ++attempt) {
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_WIFI_STATE) {
            return err;
        }
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
        if (esp_timer_get_time() >= deadline_us) {
            break;
        }
    }
    return err;
}

}  // namespace

bool CalWifiLoad(CalWifiCredentials* out) {
    nvs_handle_t handle = 0;
    const esp_err_t open_err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(kTag, "尚未配网：凭据命名空间不存在");
        return false;
    }
    // 除「命名空间不存在」以外的错误都是故障，就地终止。
    ESP_ERROR_CHECK(open_err);

    CalWifiCredentials cred;
    size_t length = sizeof(cred.ssid);
    esp_err_t err = nvs_get_str(handle, kKeySsid, cred.ssid, &length);
    if (err == ESP_OK) {
        length = sizeof(cred.pass);
        err = nvs_get_str(handle, kKeyPass, cred.pass, &length);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(kTag, "凭据不完整：%s", esp_err_to_name(err));
        return false;
    }
    if (cred.ssid[0] == '\0') {
        ESP_LOGW(kTag, "凭据存在但 ssid 为空");
        return false;
    }
    *out = cred;
    // 日志不记录密码明文，只表明是否设置了密码。
    ESP_LOGI(kTag, "已读取凭据 ssid=%s pass=%s", cred.ssid,
             cred.pass[0] != '\0' ? "<已设置>" : "<空>");
    return true;
}

esp_err_t CalWifiSave(const CalWifiCredentials& in) {
    const size_t ssid_len = std::strlen(in.ssid);
    const size_t pass_len = std::strlen(in.pass);
    if (ssid_len == 0 || ssid_len > kCalWifiSsidMax) {
        ESP_LOGE(kTag, "ssid 长度非法：%u", static_cast<unsigned>(ssid_len));
        return ESP_ERR_INVALID_ARG;
    }
    if (pass_len > kCalWifiPassMax) {
        ESP_LOGE(kTag, "密码超长：%u", static_cast<unsigned>(pass_len));
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "打开凭据命名空间失败：%s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(handle, kKeySsid, in.ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, kKeyPass, in.pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "写入凭据失败：%s", esp_err_to_name(err));
        return err;
    }

    // 回读比对：写入成功但内容不一致，说明存储层出了问题，必须让调用方看到。
    CalWifiCredentials readback;
    if (!CalWifiLoad(&readback) || std::strcmp(readback.ssid, in.ssid) != 0 ||
        std::strcmp(readback.pass, in.pass) != 0) {
        ESP_LOGE(kTag, "凭据回读比对不一致");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(kTag, "凭据已写入并校验通过");
    return ESP_OK;
}

esp_err_t CalWifiErase() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGW(kTag, "凭据已清除：%s", esp_err_to_name(err));
    return err;
}

bool CalWifiConnect(const CalWifiCredentials& cred, uint32_t timeout_ms,
                    CalWifiLink* link) {
    if (cred.ssid[0] == '\0') {
        ESP_LOGE(kTag, "ssid 为空，无法连接");
        return false;
    }
    const int64_t began_us = esp_timer_get_time();
    g_last_disconnect_reason = 0;
    if (!EnsureRadio()) {
        return false;
    }

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), cred.ssid,
                 sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(config.sta.password), cred.pass,
                 sizeof(config.sta.password) - 1);
    // 空密码按开放网络处理；有密码时要求达到 WPA2，兼容 WPA2/WPA3 过渡模式。
    config.sta.threshold.authmode =
        cred.pass[0] != '\0' ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    // 写入前留 500 ms 让上一轮关联收尾；仍在 connecting 时由 ApplyStaConfig 负责
    // disconnect 后重试，不再像原来那样一次失败就放弃整次连接。
    esp_err_t err = ApplyStaConfig(config, 500);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "设置 STA 配置失败：%s", esp_err_to_name(err));
        return false;
    }

    const int64_t deadline_us = began_us + static_cast<int64_t>(timeout_ms) * 1000;
    constexpr size_t kAttempts = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);
    for (size_t attempt = 0; attempt < kAttempts; ++attempt) {
        const int64_t remaining_ms = (deadline_us - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) {
            break;
        }
        if (kBackoffMs[attempt] > 0) {
            vTaskDelay(pdMS_TO_TICKS(
                std::min<uint32_t>(kBackoffMs[attempt], static_cast<uint32_t>(remaining_ms))));
        }
        // 先清位再发起，避免把上一轮的断开事件当成这一轮的结果。
        xEventGroupClearBits(g_events, kBitConnected | kBitDisconnected);
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(kTag, "第 %u 次发起关联失败：%s", static_cast<unsigned>(attempt + 1),
                     esp_err_to_name(err));
        }
        const int64_t wait_ms = (deadline_us - esp_timer_get_time()) / 1000;
        const EventBits_t bits =
            xEventGroupWaitBits(g_events, kBitConnected | kBitDisconnected, pdTRUE, pdFALSE,
                                pdMS_TO_TICKS(std::max<int64_t>(wait_ms, 1)));
        if ((bits & kBitConnected) != 0) {
            if (link != nullptr) {
                FillLinkInfo(began_us, link);
                ESP_LOGI(kTag, "已连接 ssid=%s ip=%s rssi=%d 耗时 %d ms", cred.ssid, link->ip,
                         link->rssi, link->elapsed_ms);
            }
            return true;
        }
        ESP_LOGW(kTag, "第 %u 次关联未成功", static_cast<unsigned>(attempt + 1));
    }
    ESP_LOGE(kTag, "连接 %s 失败，超时 %u ms，断开原因 %d：%s", cred.ssid,
             static_cast<unsigned>(timeout_ms), g_last_disconnect_reason,
             DisconnectReasonText(g_last_disconnect_reason));
    // 失败也主动断开：让 sta 立刻回到 idle，下次调用不必再等状态机收尾。
    esp_wifi_disconnect();
    return false;
}

bool CalWifiStartAp(char* ssid_out, size_t ssid_size) {
    if (ssid_out == nullptr || ssid_size < 20) {
        return false;
    }
    ssid_out[0] = '\0';
    if (!EnsureRadio()) {
        return false;
    }

    uint8_t mac[6] = {};
    const esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (mac_err != ESP_OK) {
        ESP_LOGE(kTag, "读取 MAC 失败：%s", esp_err_to_name(mac_err));
        return false;
    }
    std::snprintf(ssid_out, ssid_size, "Zectrix-Cal-%02X%02X", mac[4], mac[5]);

    // AP 网络接口要在切到 APSTA 之前创建，否则会漏掉 AP_START 事件。
    if (g_ap_netif == nullptr) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
        if (g_ap_netif == nullptr) {
            ESP_LOGE(kTag, "创建 AP 网络接口失败");
            return false;
        }
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.ap.ssid), ssid_out,
                 sizeof(config.ap.ssid));
    config.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid_out));
    config.ap.channel = 1;
    config.ap.max_connection = 2;
    // 配网窗口只有几分钟，开放热点省掉一次输密码的操作。
    config.ap.authmode = WIFI_AUTH_OPEN;
    config.ap.beacon_interval = 200;
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "配置 SoftAP 失败：%s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "SoftAP 已启动 ssid=%s 地址=192.168.4.1", ssid_out);
    return true;
}

void CalWifiStopAp() {
    if (!g_started) {
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(kTag, "SoftAP 已停止");
}

int CalWifiLastDisconnectReason() {
    return g_last_disconnect_reason;
}

void CalWifiShutdown() {
    if (!g_started) {
        return;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiEventHandler);
    // 网络接口要在 esp_wifi_deinit 之前销毁，避免事件处理器访问已释放的驱动。
    if (g_ap_netif != nullptr) {
        esp_netif_destroy_default_wifi(g_ap_netif);
        g_ap_netif = nullptr;
    }
    if (g_netif != nullptr) {
        esp_netif_destroy_default_wifi(g_netif);
        g_netif = nullptr;
    }
    esp_wifi_deinit();
    g_started = false;
    g_inited = false;
    // 深度睡眠之前必须走到这里：射频未关闭时静态电流会从几十微安升到数百微安。
    ESP_LOGI(kTag, "射频已关闭");
}
