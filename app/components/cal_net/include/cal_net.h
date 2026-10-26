#ifndef CAL_NET_H_
#define CAL_NET_H_

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// 上限取自 802.11 规范：SSID 最长 32 字节，密码最长 64 字节。
constexpr size_t kCalWifiSsidMax = 32;
constexpr size_t kCalWifiPassMax = 64;

struct CalWifiCredentials {
    char ssid[kCalWifiSsidMax + 1] = {};
    char pass[kCalWifiPassMax + 1] = {};
};

struct CalWifiLink {
    char ip[16] = {};
    int rssi = 0;
    int elapsed_ms = 0;
};

// 凭据存放在 NVS 命名空间 cal_net，键 ssid 与 pass。
// 读取失败或凭据不完整时返回 false，out 内容不保证有效。
bool CalWifiLoad(CalWifiCredentials* out);
// 写入后立刻回读比对，不一致返回 ESP_ERR_INVALID_STATE。
esp_err_t CalWifiSave(const CalWifiCredentials& in);
esp_err_t CalWifiErase();

// 连接 STA。成功返回 true 并填写 link；超时或失败返回 false。
bool CalWifiConnect(const CalWifiCredentials& cred, uint32_t timeout_ms,
                    CalWifiLink* link);

// 最近一次关联失败的原因码（wifi_err_reason_t）。0 表示没收到断开事件，
// 常见值：15 密码错、201 扫描不到 AP、204 握手超时、205 连接失败。
int CalWifiLastDisconnectReason();

// 关闭射频并释放 netif，进入深度睡眠之前必须调用。
void CalWifiShutdown();

// 启动 SoftAP 供配网使用，实际 SSID 写入 ssid_out（形如 Zectrix-Cal-3F2A）。
bool CalWifiStartAp(char* ssid_out, size_t ssid_size);
// 收回 SoftAP，射频仍在，供连接流程使用。
void CalWifiStopAp();

// 配网模式下的两行屏幕提示，由调用方负责绘制与刷新。
using CalProvisionDisplay = void (*)(const char* line1, const char* line2);

// 启动 SoftAP 与配置页面，凭据写入成功后立即重启设备。
// idle_timeout_ms 是**闲置**超时：收到串口字节或网页请求就重新计时，
// 只要还在操作就不会被踢。另设三倍时长的硬上限兜住忘关的情形。
// 成功路径不返回。
bool CalProvisionRun(uint32_t idle_timeout_ms, CalProvisionDisplay display);

// ---------------------------------------------------------------- 外部接口凭据
//
// 排班取数要的四项（AGC Client ID / Client Secret、云函数 apiKey、分组编号）。
// 存在 NVS 的 cal_agc 命名空间里，**不编进代码**：Client Secret 与 apiKey 是
// 唯一能换到排班数据的东西，写进二进制就等于随固件一起发出去。
// NVS 也没开加密 —— 那要烧 eFuse，是不可逆操作（项目红线第 1 条）。
//
// 长度上限按实测值留足余量：Client ID 19 位、Client Secret 64 位十六进制、
// apiKey 23 位、groupId 23 位。
constexpr size_t kCalApiClientIdMax = 40;
constexpr size_t kCalApiClientSecretMax = 80;
constexpr size_t kCalApiApiKeyMax = 64;
constexpr size_t kCalApiGroupIdMax = 40;

struct CalApiCredentials {
    char client_id[kCalApiClientIdMax + 1] = {};
    char client_secret[kCalApiClientSecretMax + 1] = {};
    char api_key[kCalApiApiKeyMax + 1] = {};
    char group_id[kCalApiGroupIdMax + 1] = {};
};

// 四项齐全才返回 true。不齐全说明还没配好，取数直接跳过。
bool CalApiCredLoad(CalApiCredentials* out);

// 合并写入：patch 里为空串的字段保持 NVS 里原值（配网页只改一项时不必重填其余项）。
// 写入后立刻回读比对，不一致返回 ESP_ERR_INVALID_STATE。
// complete 输出合并之后是否四项齐全，供配网界面提示。
// ⚠️ 成功写入时**连同访问令牌一起清掉** —— 令牌是按 clientId/secret 换来的，
// 换了凭证还用旧令牌，要么 401，要么拿到别人的数据。
esp_err_t CalApiCredMerge(const CalApiCredentials& patch, bool* complete);
esp_err_t CalApiCredErase();

// ---------------------------------------------------------------- 访问令牌缓存
//
// AGC 令牌 `expires_in` 实测 175480 秒（约 48.7 小时），设备一天醒一次 ——
// 每次唤醒都重新换一遍等于把一次 TLS 握手变成两次，唤醒时长和耗电都翻倍。
// 过期时刻按「本地时间 + expires_in」存成绝对时刻，判据见
// `cal_sched_token_usable()`（cal_sched.h）。
//
// 与凭证同放 `cal_agc` 命名空间：它们同属鉴权状态，分开存反而容易漏清。
constexpr size_t kCalApiTokenMax = 512;

// 读到可用令牌返回 true。token 与 expire_epoch 至少一个为空/为 0 时返回 false。
bool CalApiTokenLoad(char* token, size_t token_size, int64_t* expire_epoch);
esp_err_t CalApiTokenSave(const char* token, int64_t expire_epoch);
esp_err_t CalApiTokenClear();

#endif  // CAL_NET_H_
