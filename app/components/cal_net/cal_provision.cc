// 配网通道：USB 串口行协议与 SoftAP 网页并行运行，两条路共用同一份写入实现。
//
// 两块内容，可只给其中一块：
//   WiFi —— {"wifi":{"ssid":"..","pass":".."}}，先把凭据写进 NVS 再关联；
//           关联失败不清凭据，下次重启按已配网继续跑（只是连不上）
//   取数凭证 —— {"agc":{"clientId":"..","secret":"..","apiKey":"..","groupId":".."}}
//               留空的字段保持原值
#include "cal_net.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "cal_prov";
constexpr size_t kLineMax = 512;
constexpr uint32_t kConnectTimeoutMs = 20000;

httpd_handle_t g_server = nullptr;
std::atomic<bool> g_provision_done{false};
// 最后一次活动的时刻。串口与网页两条通道都会刷新它，用于闲置超时。
std::atomic<int64_t> g_last_activity_ms{0};
CalWifiLink g_link;
char g_connected_ssid[kCalWifiSsidMax + 1] = {};

void MarkActivity() {
    g_last_activity_ms.store(esp_timer_get_time() / 1000, std::memory_order_relaxed);
}

// 页面只做一件事：把设备自己输不进去的东西收下来。设备只有两个按键、
// 一块单色屏，手机浏览器是唯一能舒服输入长串密钥的入口。
constexpr char kIndexHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>台历配置</title>
<style>
body{font-family:system-ui,-apple-system,"Microsoft YaHei",sans-serif;margin:0;padding:24px;background:#f4f4f5;color:#18181b}
h1{font-size:20px;margin:0 0 8px}
h2{font-size:15px;margin:28px 0 4px;padding-top:16px;border-top:1px solid #d4d4d8}
p.hint{font-size:13px;color:#71717a;margin:0;line-height:1.6}
label{display:block;margin:14px 0 6px;font-size:14px;color:#3f3f46}
input{width:100%;box-sizing:border-box;padding:12px;font-size:16px;border:1px solid #d4d4d8;border-radius:8px;background:#fff}
button{width:100%;margin-top:28px;padding:14px;font-size:16px;border:0;border-radius:8px;background:#18181b;color:#fff}
button:disabled{background:#a1a1aa}
#msg{margin-top:18px;font-size:14px;line-height:1.7;white-space:pre-wrap;color:#3f3f46}
</style>
</head>
<body>
<h1>台历配置</h1>
<p class="hint">两块可以只填一块。留空的项不会改动已有内容。</p>

<h2>WiFi</h2>
<label>WiFi 名称</label>
<input id="ssid" autocomplete="off" autocapitalize="off" spellcheck="false">
<label>密码（开放网络留空）</label>
<input id="pass" type="password">

<h2>取数凭证</h2>
<p class="hint">来自 AGC 控制台与中转云函数，用于每天拉取排班。已配置过的项可以留空。</p>
<label>Client ID</label>
<input id="clientId" autocomplete="off" spellcheck="false">
<label>Client Secret</label>
<input id="secret" type="password">
<label>云函数 apiKey</label>
<input id="apiKey" type="password">
<label>分组编号 groupId</label>
<input id="groupId" autocomplete="off" spellcheck="false">

<button id="go">保存</button>
<div id="msg"></div>
<script>
const go = document.getElementById('go');
const msg = document.getElementById('msg');
const value = id => document.getElementById(id).value.trim();

go.onclick = async () => {
  const ssid = value('ssid');
  const payload = {};
  if (ssid) payload.wifi = {ssid: ssid, pass: document.getElementById('pass').value};
  const agc = {clientId: value('clientId'), secret: value('secret'),
               apiKey: value('apiKey'), groupId: value('groupId')};
  if (Object.values(agc).some(v => v)) payload.agc = agc;
  if (!payload.wifi && !payload.agc) {
    msg.textContent = '请至少填写一块内容';
    return;
  }
  go.disabled = true;
  msg.textContent = payload.wifi ? '正在连接，最多等待 20 秒…' : '正在保存…';
  try {
    const response = await fetch('/save', {
      method: 'POST',
      body: JSON.stringify(payload)
    });
    const data = await response.json();
    if (data.ok) {
      let text = payload.agc ? '取数凭证已保存' + (data.agcComplete ? '（四项齐全）' : '（还不齐全）') : '';
      if (data.ip) {
        text += (text ? '\n' : '') + 'WiFi 已连接 IP ' + data.ip +
                '　信号 ' + data.rssi + ' dBm　耗时 ' + data.ms + ' ms';
      }
      msg.textContent = text + '\n设备正在重启，可以断开热点了';
    } else {
      let text = '保存失败：' + (data.error || '未知原因');
      // 凭据落盘与关联成功分开报：密码错时输入已经存住，改一下重提即可。
      if (data.wifiSaved) {
        text += '\nWi-Fi 凭据已保存，改好密码直接再提交一次，不用从头填写。';
      }
      msg.textContent = text;
      go.disabled = false;
    }
  } catch (error) {
    msg.textContent = '无法连接设备，请确认手机仍连着台历的热点';
    go.disabled = false;
  }
};
</script>
</body>
</html>)HTML";

void SendJson(httpd_req_t* req, const char* text) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

// 配网请求解析结果。两块都可缺，但不能两块都缺。
struct ProvisionInput {
    bool has_wifi = false;
    CalWifiCredentials wifi;
    bool has_agc = false;
    CalApiCredentials agc;
};

// 一次写入的结果。stage 用于区分「解析错」「凭据写失败」「关联失败」，
// 三者排查方向完全不同，混成一个 ok=false 等于让人猜。
struct ApplyResult {
    bool ok = false;
    const char* stage = "";
    const char* error = "";
    bool agc_complete = false;
    bool wifi_saved = false;      // 凭据已落盘；关联失败时它仍是 true
    bool wifi_connected = false;  // 本次真的新连了一次，才有地址与信号可报
};

// 解析 {"wifi":{...},"agc":{...}}。ssid 必须存在且非空；agc 四个字段可缺，
// 但至少要有一个非空，否则这次提交等于什么都没做。
esp_err_t ParseProvision(const char* json, ProvisionInput* out) {
    cJSON* root = cJSON_Parse(json);
    if (root == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = ProvisionInput{};

    const cJSON* wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        const cJSON* ssid = cJSON_GetObjectItemCaseSensitive(wifi, "ssid");
        const cJSON* pass = cJSON_GetObjectItemCaseSensitive(wifi, "pass");
        if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0' ||
            std::strlen(ssid->valuestring) > kCalWifiSsidMax) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        if (cJSON_IsString(pass)) {
            if (std::strlen(pass->valuestring) > kCalWifiPassMax) {
                cJSON_Delete(root);
                return ESP_ERR_INVALID_SIZE;
            }
            std::strncpy(out->wifi.pass, pass->valuestring, kCalWifiPassMax);
        }
        std::strncpy(out->wifi.ssid, ssid->valuestring, kCalWifiSsidMax);
        out->has_wifi = true;
    }

    const cJSON* agc = cJSON_GetObjectItemCaseSensitive(root, "agc");
    if (cJSON_IsObject(agc)) {
        struct Field {
            const char* key;
            char* target;
            size_t capacity;
        };
        const Field fields[] = {
            {"clientId", out->agc.client_id, kCalApiClientIdMax},
            {"secret", out->agc.client_secret, kCalApiClientSecretMax},
            {"apiKey", out->agc.api_key, kCalApiApiKeyMax},
            {"groupId", out->agc.group_id, kCalApiGroupIdMax},
        };
        for (const Field& field : fields) {
            const cJSON* item = cJSON_GetObjectItemCaseSensitive(agc, field.key);
            if (!cJSON_IsString(item)) {
                continue;
            }
            if (std::strlen(item->valuestring) > field.capacity) {
                cJSON_Delete(root);
                return ESP_ERR_INVALID_SIZE;
            }
            std::strncpy(field.target, item->valuestring, field.capacity);
            out->has_agc = true;
        }
    }

    cJSON_Delete(root);
    if (!out->has_wifi && !out->has_agc) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

// 先写取数凭证、再连 WiFi。
//
// 顺序是有意的：写凭证不需要网络，先落盘等于先保住这次输入；关联失败也不会把
// 已经填好的凭证丢掉。反过来则要重新输一遍那两个长串。
ApplyResult ApplyProvision(const ProvisionInput& in) {
    ApplyResult result;
    if (in.has_agc) {
        bool complete = false;
        const esp_err_t err = CalApiCredMerge(in.agc, &complete);
        if (err != ESP_OK) {
            result.stage = "credential";
            result.error = esp_err_to_name(err);
            return result;
        }
        result.agc_complete = complete;
    }
    if (in.has_wifi) {
        // 先落盘、后关联（2026-10-26 修正）。原来的顺序是关联成功才写 NVS，
        // 后果是密码输错一次凭据就不保存，下次重启仍判定「缺 WiFi 凭据」→ 又进配网，
        // 形成死循环；用不存在的 SSID 配网时更是永远退不出来。
        // 关联结果只决定本次能不能报地址与信号，不该决定用户输入是否保留。
        if (CalWifiSave(in.wifi) != ESP_OK) {
            result.stage = "save";
            result.error = "wifi credential write failed";
            return result;
        }
        result.wifi_saved = true;
        if (!CalWifiConnect(in.wifi, kConnectTimeoutMs, &g_link)) {
            result.stage = "connect";
            result.error = "association failed";
            return result;
        }
        std::strncpy(g_connected_ssid, in.wifi.ssid, sizeof(g_connected_ssid) - 1);
        result.wifi_connected = true;
    }
    result.ok = true;
    return result;
}

// 串口回执以 PROV 前缀单行输出，与日志行区分，PC 侧按前缀过滤即可。
void SendReply(const ApplyResult& result) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", result.ok);
    if (!result.ok) {
        cJSON_AddStringToObject(root, "stage", result.stage);
        cJSON_AddStringToObject(root, "error", result.error);
    }
    if (result.wifi_connected) {
        cJSON_AddStringToObject(root, "ssid", g_connected_ssid);
        cJSON_AddStringToObject(root, "ip", g_link.ip);
        cJSON_AddNumberToObject(root, "rssi", g_link.rssi);
        cJSON_AddNumberToObject(root, "ms", g_link.elapsed_ms);
    }
    // 凭据落盘与关联是否成功是两件事：密码写错了 NVS 里也已经有这份输入，
    // PC 侧与网页要看得出这个区别，否则会误判成「什么都没保存」。
    cJSON_AddBoolToObject(root, "wifiSaved", result.wifi_saved);
    cJSON_AddBoolToObject(root, "agcComplete", result.agc_complete);
    char* text = cJSON_PrintUnformatted(root);
    if (text != nullptr) {
        std::printf("PROV %s\n", text);
        std::fflush(stdout);
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

esp_err_t IndexHandler(httpd_req_t* req) {
    MarkActivity();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t SaveHandler(httpd_req_t* req) {
    MarkActivity();
    char body[512] = {};
    // 一次性读完：Content-Length 已由 httpd 解析出来，分块读反而容易漏。
    // 超时次数要设上限，否则对端只发半个包时这里会一直等下去，配网窗口被卡住。
    int received = 0;
    int timeouts = 0;
    while (received < static_cast<int>(sizeof(body)) - 1) {
        const int chunk = httpd_req_recv(req, body + received, sizeof(body) - 1 - received);
        if (chunk == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                SendJson(req, "{\"ok\":false,\"stage\":\"parse\",\"error\":\"read timeout\"}");
                return ESP_OK;
            }
            continue;
        }
        if (chunk <= 0) {
            break;
        }
        received += chunk;
    }
    if (received <= 0) {
        SendJson(req, "{\"ok\":false,\"stage\":\"parse\",\"error\":\"empty request\"}");
        return ESP_OK;
    }
    body[received] = '\0';

    ProvisionInput input;
    const esp_err_t parse = ParseProvision(body, &input);
    if (parse != ESP_OK) {
        SendJson(req, "{\"ok\":false,\"stage\":\"parse\",\"error\":\"invalid request\"}");
        return ESP_OK;
    }

    const ApplyResult result = ApplyProvision(input);
    if (!result.ok) {
        // wifiSaved 让网页区分两种失败：凭据存了但连不上（改密码即可重试），
        // 与凭据根本没写进去（得先解决写入问题）。
        char reply[200] = {};
        std::snprintf(reply, sizeof(reply),
                      "{\"ok\":false,\"stage\":\"%s\",\"error\":\"%s, reason %d\",\"wifiSaved\":%s}",
                      result.stage, result.error, CalWifiLastDisconnectReason(),
                      result.wifi_saved ? "true" : "false");
        SendJson(req, reply);
        return ESP_OK;
    }

    char reply[256] = {};
    if (result.wifi_connected) {
        std::snprintf(reply, sizeof(reply),
                      "{\"ok\":true,\"ip\":\"%s\",\"rssi\":%d,\"ms\":%d,\"wifiSaved\":true,"
                      "\"agcComplete\":%s}",
                      g_link.ip, g_link.rssi, g_link.elapsed_ms,
                      result.agc_complete ? "true" : "false");
    } else {
        // 只写了取数凭证：没有新的关联，报地址只会误导。
        std::snprintf(reply, sizeof(reply),
                      "{\"ok\":true,\"wifiSaved\":%s,\"agcComplete\":%s}",
                      result.wifi_saved ? "true" : "false",
                      result.agc_complete ? "true" : "false");
    }
    SendJson(req, reply);
    g_provision_done.store(true, std::memory_order_release);
    return ESP_OK;
}

bool StartHttpd() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // 请求处理函数里要做一次最长 20 秒的关联，任务内存不能按默认值给。
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&g_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "启动网页服务失败：%s", esp_err_to_name(err));
        g_server = nullptr;
        return false;
    }

    httpd_uri_t index_uri = {};
    index_uri.uri = "/";
    index_uri.method = HTTP_GET;
    index_uri.handler = IndexHandler;
    httpd_uri_t save_uri = {};
    save_uri.uri = "/save";
    save_uri.method = HTTP_POST;
    save_uri.handler = SaveHandler;

    if (httpd_register_uri_handler(g_server, &index_uri) != ESP_OK ||
        httpd_register_uri_handler(g_server, &save_uri) != ESP_OK) {
        ESP_LOGE(kTag, "注册请求处理函数失败");
        httpd_stop(g_server);
        g_server = nullptr;
        return false;
    }
    return true;
}

void StopHttpd() {
    if (g_server == nullptr) {
        return;
    }
    httpd_stop(g_server);
    g_server = nullptr;
}

// 处理一行串口输入。返回 true 表示已保存，可以重启。
bool HandleLine(const char* line, CalProvisionDisplay display) {
    ProvisionInput input;
    const esp_err_t parse = ParseProvision(line, &input);
    if (parse != ESP_OK) {
        ESP_LOGW(kTag, "输入格式不正确：%s", esp_err_to_name(parse));
        ApplyResult failed;
        failed.stage = "parse";
        failed.error = esp_err_to_name(parse);
        SendReply(failed);
        return false;
    }
    const ApplyResult result = ApplyProvision(input);
    SendReply(result);
    if (!result.ok) {
        if (display != nullptr) {
            if (std::strcmp(result.stage, "connect") == 0) {
                // 凭据已落盘，只是这次连不上：告诉用户改密码重发即可，输入没白费。
                display("CONNECT FAILED",
                        result.wifi_saved ? "credentials saved" : "resend with correct password");
            } else {
                display("SAVE FAILED", result.stage);
            }
        }
        return false;
    }
    if (display != nullptr) {
        if (result.wifi_connected) {
            display("CONNECTED", g_link.ip);
        } else {
            display("CREDENTIALS SAVED", result.agc_complete ? "complete" : "incomplete");
        }
    }
    return true;
}

}  // namespace

bool CalProvisionRun(uint32_t idle_timeout_ms, CalProvisionDisplay display) {
    usb_serial_jtag_driver_config_t serial_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    const esp_err_t install = usb_serial_jtag_driver_install(&serial_config);
    if (install != ESP_OK && install != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "串口驱动安装失败：%s", esp_err_to_name(install));
    }
    // 驱动接管 USB-Serial/JTAG 之后，默认的 stdout 通道（ROM 直接写 FIFO）就与驱动
    // 各走一半，配网期间变成黑盒：看不见日志、也判断不出设备走到哪一步
    // （2026-10-26 实测，三次捕获全是零字节）。把 VFS 也切到驱动上，printf 与
    // ESP_LOG 才继续有效。
    //
    // 但 VFS 驱动写是 portMAX_DELAY 阻塞：没接 USB 时没有对端取数据，FIFO 一满，
    // 第一条日志就会把配网循环永久卡住。所以只在主机已连接时切换；没连接时保持
    // 默认通道（ROM 写 FIFO，无人接收则丢弃，不会阻塞）。
    const bool vfs_on_driver = usb_serial_jtag_is_connected();
    if (vfs_on_driver) {
        usb_serial_jtag_vfs_use_driver();
        ESP_LOGI(kTag, "串口驱动就绪，VFS 已切到驱动（从本行起日志正常输出）");
    }

    char ap_ssid[kCalWifiSsidMax + 1] = {};
    const bool ap_ready = CalWifiStartAp(ap_ssid, sizeof(ap_ssid));
    if (!ap_ready) {
        ESP_LOGW(kTag, "SoftAP 未启动，本次只能用串口配网");
    }
    const bool http_ready = StartHttpd();
    if (!http_ready) {
        ESP_LOGW(kTag, "网页服务未启动，本次只能用串口配网");
    }

    if (display != nullptr) {
        display(ap_ready ? ap_ssid : "USB only",
                http_ready ? "http://192.168.4.1" : "HTTP off");
    }
    ESP_LOGI(kTag, "配网等待输入，闲置 %u ms、硬上限 %u ms，ap=%s http=%s",
             static_cast<unsigned>(idle_timeout_ms),
             static_cast<unsigned>(idle_timeout_ms * 3), ap_ready ? "on" : "off",
             http_ready ? "on" : "off");

    const int64_t started_ms = esp_timer_get_time() / 1000;
    const int64_t hard_deadline_ms =
        started_ms + static_cast<int64_t>(idle_timeout_ms) * 3;
    MarkActivity();

    char line[kLineMax] = {};
    size_t used = 0;
    for (;;) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= hard_deadline_ms) {
            ESP_LOGW(kTag, "配网硬上限到期");
            break;
        }
        if (now_ms - g_last_activity_ms.load(std::memory_order_relaxed) >=
            static_cast<int64_t>(idle_timeout_ms)) {
            ESP_LOGW(kTag, "配网闲置超时");
            break;
        }

        if (g_provision_done.load(std::memory_order_acquire)) {
            StopHttpd();
            ESP_LOGI(kTag, "网页配置完成 ssid=%s ip=%s", g_connected_ssid, g_link.ip);
            if (display != nullptr) {
                if (g_connected_ssid[0] != '\0') {
                    display("CONNECTED", g_link.ip);
                } else {
                    display("SAVED", "restarting");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(800));
            std::fflush(stdout);
            esp_restart();
        }

        char byte = 0;
        const int count = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(200));
        if (count <= 0) {
            continue;
        }
        MarkActivity();
        if (byte == '\r') {
            continue;
        }
        if (byte != '\n') {
            if (used + 1 < sizeof(line)) {
                line[used++] = byte;
            } else {
                // 超长输入整行丢弃，避免缓冲区越界。
                used = 0;
                ESP_LOGW(kTag, "输入行超长，已丢弃");
            }
            continue;
        }
        line[used] = '\0';
        used = 0;
        if (line[0] == '\0') {
            continue;
        }
        if (HandleLine(line, display)) {
            StopHttpd();
            vTaskDelay(pdMS_TO_TICKS(500));
            ESP_LOGI(kTag, "串口配置完成，重启进入正常流程");
            std::fflush(stdout);
            esp_restart();
        }
    }

    StopHttpd();
    CalWifiStopAp();
    // 配网没成功、主流程还要继续跑：切回非阻塞写。否则 USB 一拔，下一条日志就会
    // 卡在 portMAX_DELAY 上，把整次唤醒堵死（比黑盒更糟）。
    if (vfs_on_driver) {
        usb_serial_jtag_vfs_use_nonblocking();
    }
    return false;
}
