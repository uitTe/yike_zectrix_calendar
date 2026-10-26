# 12 · WiFi 配网与连接

编写日期：2026年10月23日，2026年10月26日增补取数凭证与网页配网验证。

## 1. 范围

配网收两块内容：**WiFi 凭据**（连什么网）与**取数凭证**（AGC Client ID / Client Secret、云函数 apiKey、分组编号）。两块都可以只写其中一块，留空的字段不覆盖已有值。

设备只有 OK（GPIO0）与 UP（GPIO39）两个按键、一块 400×300 单色屏，逐字符输入 8 位以上的密码或 64 位十六进制密钥不成立，因此这些内容只能从外部写入。两条外部通道：USB 串口与手机网页。

两类凭据缺任一类都会进配网窗口：缺 WiFi 只能重连，缺取数凭证只补凭证。

## 2. 凭据存放

WiFi 凭据：

| 项 | 值 |
|---|---|
| 存储位置 | NVS，命名空间 `cal_net` |
| 键名 | `ssid`、`pass` |
| 长度上限 | SSID 32 字节，密码 64 字节（802.11 规范上限） |
| 写入校验 | 写入并 `nvs_commit` 后立即回读比对，不一致返回 `ESP_ERR_INVALID_STATE` |
| 驱动侧存储 | `esp_wifi_set_storage(WIFI_STORAGE_RAM)` |

取数凭证：

| 项 | 值 |
|---|---|
| 存储位置 | NVS，命名空间 `cal_agc` |
| 键名 | `clientid`、`secret`、`apikey`、`groupid` |
| 长度上限 | Client ID 40、Client Secret 80、apiKey 64、groupId 40（实测分别为 19 / 64 / 23 / 23） |
| 写入方式 | `CalApiCredMerge()`，空串字段保持原值；写入并 `nvs_commit` 后回读比对 |
| 齐全判据 | 四项都非空才算配好，缺任一项 `CalApiCredLoad()` 返回 false 并打印缺哪几项 |
| 令牌缓存 | 同命名空间另存 `token` 与 `expire`（绝对过期时刻）。换过凭证就清掉，见 `docs/11` §9.4 |

驱动侧必须设成 `WIFI_STORAGE_RAM`。默认的 `WIFI_STORAGE_FLASH` 会让 `esp_wifi_set_config()` 另写一份到 `nvs.net80211`，同一份凭据在两个地方各存一份，改了一处另一处还是旧值，排查成本很高。

**凭据是明文。** 项目红线是不烧 eFuse，因此 Flash Encryption 与基于 eFuse 的 NVS 加密都不可用，`esptool read_flash` 能读到明文。风险靠权限边界控制：AGC 应用身份只具备调用本中转函数的权限，原始班次数据源的凭据只放在云函数环境变量里，不下发到设备（见 `docs/07` 第 6 节）。日志里只打印 Client ID 与分组编号，Secret 与 apiKey 从不出现。

本文的命令与报文示例中，`<client-id>`、`<client-secret>`、`<api-key>`、`<group-id>` 是脱敏占位符，真实值只保存在设备 NVS 与 `tools/agc_probe.py` 中。

## 3. 进入配网的情形

| 情形 | 触发方式 | 串口日志 |
|---|---|---|
| 首次开机，或 WiFi 凭据被清除 | `CalWifiLoad()` 读不到 `ssid` 或 `pass` | `进入配网模式（缺 WiFi 凭据）` |
| 取数凭证不齐 | `CalApiCredLoad()` 返回 false | `进入配网模式（缺取数凭证）` |
| 两类都没有 | 上面两条同时成立 | `进入配网模式（WiFi 与取数凭证都没有）` |
| 主动重新配网 | 上键一直按住到 8 秒 | `UP held 8000 ms: restart into provisioning` |

主动触发通过 RTC 内存传递意图：`RTC_NOINIT_ATTR uint32_t g_provision_request`，魔数 `0x50524F56`。软件复位不清这块内存，上电复位时内容不确定，所以启动时必须校验魔数，读到后立刻清零。

上键是两段式的，重启刷新的原有语义不变：

| 手势 | 行为 |
|---|---|
| 按住 3 秒到 8 秒之间松开 | `esp_restart()` 重启刷新 |
| 一直按住到 8 秒 | 置魔数后 `esp_restart()`，重启进配网，不必松开 |
| 配网期间**短按 BOOT** | 退出配网并重启，凭据已在 NVS，重启后直接显示台历页 |

配网窗口按**闲置**计时：300 秒内没有串口输入也没有网页请求才退出，另设 900 秒硬上限兜住忘关的情形。只要还在操作就一直续期。退出后按未配网继续运行，台历页照常刷新，不影响离线历法。

配网期间按键仍然生效：上键按满 3 秒松开走重启刷新分支，一直按住到 8 秒则重新进入配网。**想中途退出，短按 BOOT**——上键那条 3 秒判定有 ±40 ms 的采样误差，按数到 3 秒常常差几十毫秒落在阈值下方，按久一点又会撞上 8 秒分支重启后仍停在配网页；BOOT 短按只看「按下到松开不到 500 ms」，不受时长阈值影响。

## 4. 串口配网

设备端在配网窗口内循环读取 USB Serial/JTAG 的整行文本，按 `\n` 切分，单行上限 255 字节，超出整行丢弃并打警告。

### 4.1 协议

发送（只写 WiFi）：

```json
{"wifi":{"ssid":"MyWiFi","pass":"12345678"}}
```

发送（只写取数凭证，或补其中几项）：

```json
{"agc":{"clientId":"<client-id>","secret":"<client-secret>","apiKey":"<api-key>","groupId":"<group-id>"}}
```

`ssid` 必填且非空，`pass` 可省略（开放网络）。中文 SSID 按 UTF-8 字节写入。`agc` 四个字段可任意缺省，缺失或空串的项保持 NVS 原值；两块也可以一次全给。两块都没给按 `parse` 失败处理。

成功回执（单行，`PROV ` 前缀）：

```
PROV {"ok":true,"ssid":"MyWiFi","ip":"192.168.1.7","rssi":-51,"ms":1842,"agcComplete":true}
```

只写取数凭证时没有新的关联，回执不带地址：

```
PROV {"ok":true,"agcComplete":true}
```

失败回执：

```
PROV {"ok":false,"stage":"connect","error":"association failed","agcComplete":false}
```

| 字段 | 说明 |
|---|---|
| `stage` | `parse` 输入格式错误、`credential` 取数凭证写入失败、`save` WiFi 凭据写入失败、`connect` 关联失败 |
| `ip` / `rssi` / `ms` | 仅在新发起关联时出现 |
| `agcComplete` | 合并之后取数凭证是否四项齐全 |

回执用 `PROV ` 前缀与日志行区分，PC 侧按前缀过滤即可。密码在任何日志里都不出现明文，读凭据时只打印 `<已设置>` 或 `<空>`。

### 4.2 操作

```
tools\.venv\Scripts\python.exe tools\provision.py --list
tools\.venv\Scripts\python.exe tools\provision.py --port COM5 --ssid MyWiFi --pass 12345678
tools\.venv\Scripts\python.exe tools\provision.py --port COM5 ^
    --agc-client-id <client-id> --agc-secret <client-secret> ^
    --agc-api-key <api-key> --agc-group-id <group-id>
```

| 参数 | 说明 |
|---|---|
| `--port` | 串口设备，设备插上后通常是 COM5 |
| `--ssid` / `--pass` | WiFi 名称与密码，开放网络省略 `--pass` |
| `--agc-client-id` 等四项 | 取数凭证，只给需要改的项 |
| `--json` | 直传完整请求体，优先于其它参数 |
| `--timeout` | 等待回执的秒数，默认 30（关联本身最长 20 秒） |
| `--list` | 只枚举本机串口，不发送 |

脚本**不碰 DTR 与 RTS**。ESP32-S3 的 USB-Serial/JTAG 把 DTR 映射到芯片 EN 引脚，拉低产生的下降沿等于按一下复位，设备会在脚本写入之前重启，表现是 `ClearCommError` 或 `PermissionError(13, '设备不识别此命令。')`。打开串口后只等 0.3 秒让设备稳定，再清输入缓冲。

## 5. 网页配网

1. 手机连接热点 `Zectrix-Cal-XXXX`，后四位取自 SoftAP 的 MAC
2. 打开浏览器，访问 `http://192.168.4.1`
3. 页面分「WiFi」与「取数凭证」两块，只填需要改的项，点「保存」
4. 页面显示保存结果（连着 WiFi 时还显示 IP、信号强度与耗时），设备随后自动重启

热点是开放的，只在配网窗口内存在，窗口结束即关闭。屏幕上第 2 行是热点名、第 3 行是这个地址。

**不做 DNS 劫持。** 那需要自己构造 UDP 53 的应答报文，项目规范禁止手写协议解析。代价是手机连上后不会自动弹出强制门户，需要手动在浏览器输入地址。

网页与串口并行运行，谁先完成都可以。SoftAP 或网页服务启动失败时只打警告，另一条通道照常可用，不会让整个配网流程失败。

## 6. 连接流程

`CalWifiConnect()` 的执行顺序：

| 步骤 | 要点 |
|---|---|
| 1 | `esp_netif_init()`、`esp_event_loop_create_default()`，返回 `ESP_ERR_INVALID_STATE` 视为已初始化 |
| 2 | `esp_netif_create_default_wifi_sta()`、`esp_wifi_init()`、`esp_wifi_set_storage(RAM)` |
| 3 | 注册 `WIFI_EVENT` 与 `IP_EVENT_STA_GOT_IP` 处理器，**必须在 `esp_wifi_start()` 之前** |
| 4 | `esp_wifi_set_mode(WIFI_MODE_STA)`、`esp_wifi_start()`，等 `WIFI_EVENT_STA_START`，超时 5 秒 |
| 5 | `esp_wifi_set_config()` 后发起关联，只认 `IP_EVENT_STA_GOT_IP` |
| 6 | 三次退避，间隔 0 / 1 / 3 秒，总时长受调用方给的超时约束（配网用 20 秒） |
| 7 | 采集 `esp_wifi_sta_get_ap_info()` 的 RSSI 与 `esp_netif_get_ip_info()` 的地址 |

`WIFI_EVENT_STA_CONNECTED` 时还没有可用地址，不能当作成功。每次重试前先清事件位，避免把上一轮的断开事件当成这一轮的结果。

密码为空时 `threshold.authmode` 用 `WIFI_AUTH_OPEN`，否则用 `WIFI_AUTH_WPA2_PSK`，并设 `pmf_cfg.capable = true` 兼容 WPA2/WPA3 过渡模式。

## 7. 与深度睡眠的关系

ESP32-S3 进入深度睡眠时 modem 电源域整体断电，只有 RTC 域存活。**深度睡眠期间无法保持 WiFi 连接**，每次唤醒都是一次冷启动，射频、DHCP 租约、TLS 会话全部重来。

| 阶段 | 每次唤醒都要重新做 | 耗时 |
|---|---|---|
| 初始化射频与 netif | 是 | 0.3–0.6 s |
| 关联 AP | 是 | 0.8–1.5 s |
| DHCP 取得地址 | 是 | 0.3–0.8 s |
| 合计 | | 约 1.5–3 s，2026-10-23 实测 1668 ms |

按 `docs/03` §6.4 的日预算，这笔开销约 0.05–0.1 mAh，占 2.76 mAh 的 2–4%，可以接受。

**睡前必须调用 `CalWifiShutdown()`**，位置在 `rtc_gpio_hold_en(GPIO17)` 之前、`SetAudioPower(false)` 之前。射频没关就直接进深睡，静态电流会从几十微安升到数百微安。关闭顺序是 `disconnect` → `stop` → 注销事件处理器 → 销毁 netif → `esp_wifi_deinit()`；netif 必须在 `deinit` 之前销毁，否则事件处理器会访问已释放的驱动。

## 8. 失败处置

| 现象 | 可能原因 | 处置 |
|---|---|---|
| 设备不进配网 | NVS 里凭据齐全 | 上键按住到 8 秒 |
| 串口无回执 | 设备不在配网窗口、串口被其他程序占用、USB 线只供电不传数据 | 检查日志是否出现 `配网等待输入`；关掉串口助手后重试 |
| `stage:"parse"` | JSON 结构不对，或 ssid 超长 | 按 4.1 的结构重发 |
| `stage:"credential"` | 取数凭证写入 NVS 失败（长度超限或 NVS 异常） | 看日志里的具体错误；四项长度上限见第 2 节 |
| `stage:"save"` | WiFi 凭据写入 NVS 失败 | 看日志里的具体错误；NVS 损坏时启动会打印 `NVS 需要重建`，凭据会丢，需要重新配网 |
| `stage:"connect"` | 密码错误、AP 不在范围、路由器只允许 WPA3 纯模式 | 看回执里的 `reason` 码，见下表；设备已保留凭据，用正确密码重发即可覆盖 |
| `reason 201` | 2.4 GHz 信道上扫不到该 SSID（`NO_AP_FOUND`） | 该网络只在 5 GHz 广播，或路由器的 2.4G 被关闭；换 2.4G 的名字 |
| `reason 15` | 四次握手超时（`4WAY_HANDSHAKE_TIMEOUT`） | 密码错。此时 AP 已扫到并完成关联，链路本身正常 |
| `reason 204` | 握手超时（`HANDSHAKE_TIMEOUT`） | 密码或加密方式不匹配，确认 AP 不是 WPA3 纯模式 |
| 手机打不开 `192.168.4.1` | 配网窗口已过、热点未启动 | 看屏幕第二行是否有地址；重新进配网 |
| 配网成功后重启仍连不上 | 路由器 DHCP 分配慢或信号弱 | 查日志里的 `rssi`；低于 -80 dBm 时需要挪位置 |

## 9. 代码与工具位置

| 内容 | 路径 |
|---|---|
| 对外接口 | `app/components/cal_net/include/cal_net.h` |
| WiFi 凭据存取与连接 | `app/components/cal_net/cal_wifi.cc` |
| 取数凭证存取 | `app/components/cal_net/cal_cred.cc` |
| 串口与网页配网 | `app/components/cal_net/cal_provision.cc` |
| 进入配网的判定与提示屏 | `app/main/app_main.cc`（`EnsureProvisioned`、`ShowProvisionScreen`） |
| PC 侧串口工具 | `tools/provision.py` |

组件依赖：`REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer`，`PRIV_REQUIRES esp_http_server lwip json esp_driver_usb_serial_jtag`。

## 10. 验证状态

已完成（离线）：

- 全量编译通过，`zectrix_calendar.bin` 1 262 240 字节（含取数与 TLS 根证书包），app 分区剩余 60%。
- 符号表核对，`CalWifiLoad`、`CalWifiSave`、`CalWifiConnect`、`CalWifiStartAp`、`CalWifiStopAp`、`CalProvisionRun`、`IndexHandler`、`SaveHandler` 全部进入镜像。
- 内嵌页面结构与脚本语法校验通过（`app/build/extract_page.py`，`node --check`）。
- `tools/provision.py` 的 `--list`、`--help` 与语法检查通过。

已完成（设备实测，2026-10-23）：

| 项 | 结果 | 证据 |
|---|---|---|
| 系统时钟灌入 | 通过 | `system clock seeded from rtc: local 2026-10-23 17:07:36` |
| 无凭据自动进配网 | 通过 | `尚未配网：凭据命名空间不存在` → `进入配网模式（尚无凭据）` |
| SoftAP 启动 | 通过 | `SoftAP 已启动 ssid=Zectrix-Cal-XXXX 地址=192.168.4.1` |
| 串口写入并关联成功 | 通过 | `{"ok":true,"ssid":"MyWiFi2.4G","ip":"192.168.1.7","rssi":-51,"ms":1668}` |
| 上键按住进配网 | 通过 | `UP held 6000 ms: restart into provisioning`（当时阈值 6 秒，现为 8 秒） |
| 上键 3 秒松开仍是重启 | 通过 | `UP long press released: restart`，未置魔数，重启后跳过配网 |
| 重启后凭据读回 | 通过 | `已读取凭据 ssid=MyWiFi pass=<已设置>` |
| 有凭据时跳过配网 | 通过 | 第二次启动无 `进入配网模式`，直接跑自检与渲染 |
| 原因码诊断 | 通过 | 5 GHz 专用网络报 `reason 201`，密码错误的 2.4G 网络报 `reason 15` |

实测关联指标：从发起连接到取得地址 **1668 ms**，信号 **-51 dBm**，地址 **192.168.1.7**。这个耗时落在第 7 节的预算区间内，其中关联与 DHCP 合计约 1.6 秒。

设备实测记录里的 SSID、内网地址与软热点名已替换为示例值。

已完成（设备实测，2026-10-26）：

| 项 | 结果 | 证据 |
|---|---|---|
| 串口写入取数凭证 | 通过 | 回执 `PROV {"ok":true,"agcComplete":true}`，重启后不再进配网（`docs/11` §9.8） |
| 手机网页配网 | 通过 | 手机连接热点 `Zectrix-Cal-XXXX` 后访问 `http://192.168.4.1`，在页面上保存 WiFi 与取数凭证，设备重启 |

待验证：

- 睡前射频关闭后的静态电流。
