// AGC 取数：OAuth 换 token → 调中转云函数 → 写缓存。
//
// 接口真源 `docs/11-AGC接口调用说明.md`（2026-10-22 实测成功的那一次）。三个不能改的细节：
//   ① OAuth 请求体里的 `useJwt` 是 **JSON 数字 0**，字段名大小写固定；
//   ② 云函数请求头必须同时带 `clientId` 与 `productId`，缺一个就是 403；
//   ③ 生效的云函数域名是 `connect-drcn.dbankcloud.cn`（PC 侧脚本里 `PROXY_URL_ORI` 那条），
//      不是 `connect-api.cloud.huawei.com`。
#include <cstdio>
#include <cstring>
#include <ctime>
#include <strings.h>

#include "cal_net.h"
#include "cal_sched_net.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "cal_sched";
constexpr char kTokenUrl[] =
    "https://connect-drcn.dbankcloud.cn/agc/apigw/oauth2/v1/token";
// 云函数地址与 productId 由 Kconfig 注入，源码里只留占位默认值。
// 真实值写本地 app/sdkconfig（不入库），见 app/main/Kconfig.projbuild。
constexpr char kProxyUrl[] = CONFIG_CAL_AGC_PROXY_URL;
constexpr char kProductId[] = CONFIG_CAL_AGC_PRODUCT_ID;

constexpr uint32_t kHttpTimeoutMs = 15000;  // 单次 HTTP 超时，与 PC 侧脚本一致
constexpr uint32_t kLinkTimeoutMs = 10000;  // WiFi 关联超时，实测 1.7 秒足够
constexpr int kAttempts = 3;                // docs/03 §2.4：最多 3 轮
constexpr int kRetryDelayMs = 60000;        // 轮间间隔 1 分钟
constexpr size_t kBodyMax = 4096;           // 14 天响应实测 340 字节
constexpr size_t kTokenMax = 512;           // 实测令牌 227 字符
constexpr size_t kRequestMax = 512;         // 两个请求体实测都在 250 字节以内
// 令牌复用要留的安全余量。实测 expires_in = 175480 秒（约 48.7 小时），
// 设备一天醒一次 —— 留 1 小时足够吸收时钟偏差与请求往返，又不至于让
// 「还剩 50 分钟就到期」的令牌被白白丢弃后再换一次。
constexpr int32_t kTokenMarginSeconds = 3600;
// 响应里没有 expires_in 时的保守有效期：宁可多换一次，也不拿过期令牌去调。
constexpr int64_t kFallbackTtlSeconds = 3600;

// 缓冲全部放静态区：主任务栈只有 8 KB，而这份文件里要同时放 4 KB 响应、
// 请求体、令牌和解析结果。整条链路只有主任务在跑（每日唤醒是单线程流程），
// 不存在重入，用静态区比把栈顶到边界安全。
char g_body[kBodyMax];
char g_request[kRequestMax];
char g_token[kTokenMax];
CalSchedPayload g_payload;
// 最近一次 OAuth 返回的有效期（秒）。令牌缓存用它算绝对过期时刻。
int64_t g_token_ttl_seconds = 0;
// 响应头里的 `Date`。校时唯一来源（docs/03 §6.1），必须在事件回调里抓：
// 实测 `esp_http_client_get_header()` 对它返回空值 —— 那个接口只覆盖驱动内部
// 记下来的少数几个头，`Date` 不在其中。HTTP_EVENT_ON_HEADER 能看到全部响应头。
char g_date_header[40];

esp_err_t CaptureDateHeader(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_HEADER || evt->header_key == nullptr ||
        evt->header_value == nullptr) {
        return ESP_OK;
    }
    if (strcasecmp(evt->header_key, "Date") == 0) {
        std::snprintf(g_date_header, sizeof(g_date_header), "%s", evt->header_value);
    }
    return ESP_OK;
}

struct HttpHeader {
    const char* key;
    const char* value;
};

// 一次 POST 的结果。传输失败（连不上、TLS 失败、超时）时 transport_ok 为 false；
// 传输成功但业务拒绝时 transport_ok 为 true，status 里是 HTTP 状态码 —— 这两种要分开报。
struct HttpOutcome {
    bool transport_ok = false;
    int status = 0;
    int length = 0;
    char date[40] = {};
};

bool HttpPostJson(const char* url, const char* body, const HttpHeader* headers,
                  size_t header_count, HttpOutcome* out) {
    *out = HttpOutcome{};
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = static_cast<int>(kHttpTimeoutMs);
    // 证书走 IDF 自带根证书包（CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y）。
    // 不开跳过校验：这台设备一天只醒一次，拿到错误数据比拿不到数据更糟。
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    config.user_agent = "zectrix-calendar/1";
    config.event_handler = CaptureDateHeader;

    g_date_header[0] = '\0';

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(kTag, "创建 HTTP 客户端失败");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    for (size_t i = 0; i < header_count; ++i) {
        esp_http_client_set_header(client, headers[i].key, headers[i].value);
    }

    const int body_len = static_cast<int>(std::strlen(body));
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "建立连接失败（%s）：%s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    // write 不保证一次写完，按返回值累加，避免半截 JSON 发出去后收到「解析失败」。
    int written = 0;
    while (written < body_len) {
        const int chunk = esp_http_client_write(client, body + written, body_len - written);
        if (chunk <= 0) {
            ESP_LOGE(kTag, "请求体写入中断：%d / %d", written, body_len);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        written += chunk;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        ESP_LOGE(kTag, "读取响应头失败");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    out->status = esp_http_client_get_status_code(client);
    // Date 由 CaptureDateHeader 在 fetch_headers 期间抓进 g_date_header。
    std::snprintf(out->date, sizeof(out->date), "%s", g_date_header);

    // 一次性收齐响应体。超出 kBodyMax 直接失败 —— 静默截断会让 JSON 恰好断在
    // 某一条记录中间，解析出来的结果看起来「少了一天」，很难归因。
    int used = 0;
    for (;;) {
        if (static_cast<size_t>(used) >= kBodyMax) {
            ESP_LOGE(kTag, "响应体超过 %u 字节上限", static_cast<unsigned>(kBodyMax));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        const int read =
            esp_http_client_read(client, g_body + used, static_cast<int>(kBodyMax - used));
        if (read < 0) {
            ESP_LOGE(kTag, "读取响应体失败");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (read == 0) {
            break;
        }
        used += read;
    }
    out->length = used;
    out->transport_ok = true;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return true;
}

// 把 cJSON 请求体序列化进 g_request。返回长度，失败返回 0。
size_t BuildBody(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) {
        return 0;
    }
    const size_t len = std::strlen(text);
    if (len + 1 > sizeof(g_request)) {
        cJSON_free(text);
        return 0;
    }
    std::memcpy(g_request, text, len + 1);
    cJSON_free(text);
    return len;
}

// 第一步：换 access_token。docs/11 §4.2 的请求体字段一个都不能少。
bool FetchToken(const CalApiCredentials& cred, char* date_out, size_t date_size) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "grant_type", "client_credentials");
    cJSON_AddStringToObject(root, "client_id", cred.client_id);
    cJSON_AddStringToObject(root, "client_secret", cred.client_secret);
    // useJwt 是 JSON 数字，不是字符串 —— 写成 "0" 会被 OAuth 接口拒。
    cJSON_AddNumberToObject(root, "useJwt", 0);
    if (BuildBody(root) == 0) {
        ESP_LOGE(kTag, "OAuth 请求体序列化失败");
        return false;
    }

    HttpOutcome outcome;
    const int64_t began_us = esp_timer_get_time();
    if (!HttpPostJson(kTokenUrl, g_request, nullptr, 0, &outcome)) {
        return false;
    }
    ESP_LOGI(kTag, "OAuth：HTTP %d，耗时 %d ms，响应 %d 字节", outcome.status,
             static_cast<int>((esp_timer_get_time() - began_us) / 1000), outcome.length);

    cJSON* parsed = cJSON_ParseWithLength(g_body, outcome.length);
    if (parsed == nullptr) {
        ESP_LOGE(kTag, "OAuth 响应不是合法 JSON");
        return false;
    }
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(parsed, "access_token");
    const bool ok = cJSON_IsString(item) && item->valuestring[0] != '\0' &&
                    std::strlen(item->valuestring) < sizeof(g_token);
    if (ok) {
        std::snprintf(g_token, sizeof(g_token), "%s", item->valuestring);
    } else {
        // 华为 AGC 用 HTTP 200 包装业务错误，只看状态码会漏判（docs/11 §4.4）。
        const cJSON* ret = cJSON_GetObjectItemCaseSensitive(parsed, "ret");
        const cJSON* msg = cJSON_GetObjectItemCaseSensitive(ret, "msg");
        ESP_LOGE(kTag, "OAuth 响应里没有可用 access_token（HTTP %d，ret.msg=%s）",
                 outcome.status, cJSON_IsString(msg) ? msg->valuestring : "-");
    }
    // 有效期每次都要取：响应里没有就按保守值算，不能沿用上一次的。
    // 必须在 cJSON_Delete 之前读出来。
    const cJSON* expires = cJSON_GetObjectItemCaseSensitive(parsed, "expires_in");
    const bool has_ttl = cJSON_IsNumber(expires) && expires->valuedouble > 0;
    const int64_t ttl =
        has_ttl ? static_cast<int64_t>(expires->valuedouble) : kFallbackTtlSeconds;
    cJSON_Delete(parsed);
    if (!ok) {
        return false;
    }
    g_token_ttl_seconds = ttl;
    if (!has_ttl) {
        ESP_LOGW(kTag, "OAuth 响应没有 expires_in，令牌按 %lld 秒有效期处理",
                 static_cast<long long>(kFallbackTtlSeconds));
    }
    ESP_LOGI(kTag, "OAuth 成功，令牌 %u 字符，有效期 %lld 秒",
             static_cast<unsigned>(std::strlen(g_token)),
             static_cast<long long>(g_token_ttl_seconds));
    if (date_out != nullptr && outcome.date[0] != '\0') {
        std::snprintf(date_out, date_size, "%s", outcome.date);
    }
    return true;
}

// 第二步：调中转云函数。clientId / productId / Authorization 三个头缺一不可。
// 返回 true 只表示**传输成功** —— 业务状态放在 http_status 里，由调用方判断：
// 401 要走「换令牌重试」，其他非 200 直接报错，两者处置不同。
bool FetchProxy(const CalApiCredentials& cred, const char* start_date, char* date_out,
                size_t date_size, int* body_length, int* http_status) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(root, "apiKey", cred.api_key);
    cJSON* data = cJSON_CreateObject();
    if (data == nullptr) {
        cJSON_Delete(root);
        return false;
    }
    cJSON_AddStringToObject(data, "groupId", cred.group_id);
    cJSON_AddStringToObject(data, "startDate", start_date);
    cJSON_AddItemToObject(root, "data", data);
    if (BuildBody(root) == 0) {
        ESP_LOGE(kTag, "云函数请求体序列化失败");
        return false;
    }

    char bearer[kTokenMax + 8] = {};
    std::snprintf(bearer, sizeof(bearer), "Bearer %s", g_token);
    const HttpHeader headers[] = {
        {"clientId", cred.client_id},
        {"productId", kProductId},
        {"Authorization", bearer},
    };

    HttpOutcome outcome;
    const int64_t began_us = esp_timer_get_time();
    if (!HttpPostJson(kProxyUrl, g_request, headers, sizeof(headers) / sizeof(headers[0]),
                      &outcome)) {
        return false;
    }
    ESP_LOGI(kTag, "云函数：HTTP %d，耗时 %d ms，响应 %d 字节", outcome.status,
             static_cast<int>((esp_timer_get_time() - began_us) / 1000), outcome.length);
    *body_length = outcome.length;
    *http_status = outcome.status;
    if (date_out != nullptr && outcome.date[0] != '\0') {
        // 这一步比 token 那一步晚 1 秒多，用它校准更准。
        std::snprintf(date_out, date_size, "%s", outcome.date);
    }
    return true;
}

// 令牌按需获取：有效期内直接用 NVS 里那份，不再打 OAuth。
//
// 设备一天醒一次、令牌能活两天，所以正常情况下每两次唤醒才换一次令牌 ——
// 每次唤醒少一次 TLS 握手，省的是唤醒时长（也就省电）。
bool EnsureToken(const CalApiCredentials& cred, char* date_out, size_t date_size) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    char cached[kTokenMax] = {};
    int64_t expire_epoch = 0;
    if (CalApiTokenLoad(cached, sizeof(cached), &expire_epoch) &&
        cal_sched_token_usable(expire_epoch, now, kTokenMarginSeconds)) {
        std::snprintf(g_token, sizeof(g_token), "%s", cached);
        ESP_LOGI(kTag, "复用缓存令牌，还剩 %lld 秒（%.1f 小时），跳过 OAuth",
                 static_cast<long long>(expire_epoch - now),
                 static_cast<double>(expire_epoch - now) / 3600.0);
        return true;
    }
    if (expire_epoch > 0) {
        ESP_LOGI(kTag, "缓存令牌已过期（过期时刻 %lld，当前 %lld），重新获取",
                 static_cast<long long>(expire_epoch), static_cast<long long>(now));
    }
    if (!FetchToken(cred, date_out, date_size)) {
        return false;
    }
    if (CalApiTokenSave(g_token, now + g_token_ttl_seconds) != ESP_OK) {
        // 存不下不影响本次调用，代价只是下次唤醒多换一次令牌。
        ESP_LOGW(kTag, "令牌未写入缓存，下次唤醒会重新获取");
    }
    return true;
}

bool ParseAndStore(int body_length, CalSchedUpdateResult* out);

// 取一次排班：按需换令牌 → 调云函数 →（401 时）换令牌重试一次 → 解析入库。
bool FetchOnce(const CalApiCredentials& cred, const char* start_date, char* date_out,
               size_t date_size, CalSchedUpdateResult* out) {
    if (!EnsureToken(cred, date_out, date_size)) {
        return false;
    }
    int body_length = 0;
    int status = 0;
    if (!FetchProxy(cred, start_date, date_out, date_size, &body_length, &status)) {
        return false;
    }
    if (status == 401) {
        // 401 = 第一层校验失败（令牌无效或已作废）。本地判定说「还没过期」也可能
        // 因为时钟偏差而误判，所以这里不能只报错，要清掉缓存亲手续一次。
        ESP_LOGW(kTag, "云函数返回 401：令牌被拒，丢弃缓存后重新换取");
        CalApiTokenClear();
        if (!FetchToken(cred, date_out, date_size)) {
            return false;
        }
        if (CalApiTokenSave(g_token,
                            static_cast<int64_t>(std::time(nullptr)) +
                                g_token_ttl_seconds) != ESP_OK) {
            ESP_LOGW(kTag, "令牌未写入缓存，下次唤醒会重新获取");
        }
        if (!FetchProxy(cred, start_date, date_out, date_size, &body_length, &status)) {
            return false;
        }
        if (status == 401) {
            ESP_LOGE(kTag, "重新换取令牌后仍返回 401 —— 核对 Client ID / Client Secret");
            return false;
        }
    }
    if (status != 200) {
        ESP_LOGE(kTag, "云函数拒绝请求：HTTP %d，响应体前 200 字节：%.200s", status, g_body);
        return false;
    }
    return ParseAndStore(body_length, out);
}

// 解析响应体并写入缓存。成功时整体覆盖 out->cache。
bool ParseAndStore(int body_length, CalSchedUpdateResult* out) {
    if (!cal_sched_parse(g_body, static_cast<size_t>(body_length), &g_payload)) {
        ESP_LOGE(kTag, "响应不符合数据契约：%s", cal_sched_last_error());
        return false;
    }
    ESP_LOGI(kTag, "排班查询成功：%s，%d 天", g_payload.message, g_payload.count);

    // 直接往 out->cache 里重建，不再另开一份局部缓存 —— 一份 1.1 KB，
    // 主任务栈只有 8 KB，能省一层是一层。
    cal_sched_cache_clear(&out->cache);
    out->cache.count = g_payload.count;
    for (int i = 0; i < g_payload.count; ++i) {
        out->cache.days[i] = g_payload.days[i];
        // 每天打一行，把「无排班」与「没取到」在日志里分清 —— 页面上两者都是空白。
        const CalSchedDay& day = g_payload.days[i];
        if (day.has_schedule) {
            ESP_LOGI(kTag, "  %04d-%02d-%02d %s [%s] [%s]", day.year, day.month, day.day,
                     day.shift, day.am, day.pm);
        } else {
            ESP_LOGI(kTag, "  %04d-%02d-%02d 无排班数据", day.year, day.month, day.day);
        }
    }
    if (!CalSchedCacheSave(out->cache)) {
        // 缓存写失败不影响本次显示，但断网兜底没了，必须报出来。
        ESP_LOGW(kTag, "缓存写入失败，本次显示用刚取到的数据");
    }
    out->fetched = true;
    return true;
}

// 本地日期 → YYYY-MM-DD。系统时钟已由 app_main 从 RTC 灌入（TZ=CST-8）。
bool LocalDateString(char* out, size_t out_size) {
    const time_t now = std::time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2020) {
        ESP_LOGE(kTag, "系统时间不可用（%d 年），跳过取数", local.tm_year + 1900);
        return false;
    }
    std::snprintf(out, out_size, "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday);
    return true;
}

}  // namespace

void CalSchedUpdate(const CalWifiCredentials& wifi, const CalApiCredentials& cred,
                    CalSchedUpdateResult* out) {
    if (out == nullptr) {
        return;
    }
    *out = CalSchedUpdateResult{};
    const int64_t began_us = esp_timer_get_time();

    // 先把旧缓存装进来：无论本次成败，页面至少还能用上一次的数据。
    if (!CalSchedCacheLoad(&out->cache)) {
        cal_sched_cache_clear(&out->cache);
    }

    char start_date[16] = {};
    if (!LocalDateString(start_date, sizeof(start_date))) {
        return;
    }
    ESP_LOGI(kTag, "开始取数，startDate=%s，groupId=%s", start_date, cred.group_id);

    bool connected = false;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        out->attempts = attempt + 1;
        if (!connected) {
            CalWifiLink link;
            connected = CalWifiConnect(wifi, kLinkTimeoutMs, &link);
            if (!connected) {
                // 关联失败不再额外等 1 分钟：关联本身已耗掉 10 秒并做过退避，
                // 再等只会白烧电（静态电流 50 µA，射频是 100 mA）。
                ESP_LOGW(kTag, "第 %d 轮：WiFi 关联失败，立即重试", attempt + 1);
                continue;
            }
            out->wifi_connected = true;
            out->rssi = link.rssi;
        }
        char date_text[40] = {};
        if (FetchOnce(cred, start_date, date_text, sizeof(date_text), out)) {
            int64_t epoch = 0;
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
            if (cal_sched_parse_http_date(date_text, &epoch) &&
                cal_sched_beijing_time(epoch, &year, &month, &day, &hour, &minute,
                                       &second)) {
                out->has_server_time = true;
                out->year = year;
                out->month = month;
                out->day = day;
                out->hour = hour;
                out->minute = minute;
                out->second = second;
                ESP_LOGI(kTag, "服务器时间（北京时间）%04d-%02d-%02d %02d:%02d:%02d", year,
                         month, day, hour, minute, second);
            } else {
                ESP_LOGW(kTag, "Date 头不可用（\"%s\"），本次不校准 RTC", date_text);
            }
            break;
        }
        connected = false;
        if (attempt + 1 < kAttempts) {
            ESP_LOGW(kTag, "第 %d 轮取数失败，%d 秒后重试", attempt + 1,
                     kRetryDelayMs / 1000);
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
        }
    }

    ESP_LOGI(kTag, "取数结束：%s，%d 轮，耗时 %d ms，缓存 %d 天",
             out->fetched ? "成功" : "失败", out->attempts,
             static_cast<int>((esp_timer_get_time() - began_us) / 1000), out->cache.count);
}
