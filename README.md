# Zectrix Note 4 台历固件

在 Zectrix Note 4 墨水屏设备上每天自动刷新一页台历：年月日、农历、星期、节气、节日、当日班次与两段时段、电量与 WiFi 信号。每天北京时间 00:00 由板载 RTC 闹钟唤醒，刷新后进入深度休眠，日常靠电池运行。

| 项     | 值                                                       |
| ------ | -------------------------------------------------------- |
| 硬件   | **Zectrix Note 4**（官方成品设备，非开发套件），PCB v1.0 |
| 平台   | ESP32-S3 **N16R8**（16 MB Flash / 8 MB PSRAM）           |
| 屏幕   | 4.2 英寸 400×300 黑白墨水屏，SSD2683，1bpp               |
| 工具链 | **ESP-IDF v5.4.4**                                       |

本项目是第三方固件，与 Zectrix 官方无关。**仅适用于黑白屏 NOTE4，不适用于 NOTE4C。**

## 功能

| 项           | 说明                                                                                  |
| ------------ | ------------------------------------------------------------------------------------- |
| 每日自动刷新 | PCF8563 闹钟经 GPIO5 唤醒，跨零点检测后补刷，不依赖 USB 与电脑                        |
| 历法         | 农历（1900–2099）、二十四节气（天文算法，零数据表）、公历与农历节日；同日多个节日并排 |
| 班次         | 每天拉取未来 14 天滚动窗口缓存到 NVS，断网仍能正确显示；未排班与拉取失败分开处理      |
| 电量与信号   | 电池图标 + 5% 步进百分比 + 四格 WiFi 强度；低于 20% 加感叹号，充电显示闪电            |
| 按键         | BOOT 短按唤醒；上键长按 3 秒复位刷新，按到 8 秒进配网；下键不接管                     |
| 配网         | USB 串口写 JSON，或手机连热点后在网页填写                                             |
| 省电         | 深度休眠并锁存电池供电轨；每次唤醒取数约 4–9 秒，其余时间静态待机                     |

不配置取数凭证也能用：历法、电量、按键全部离线工作，班次区显示「获取失败」。

## 环境要求

| 项                 | 版本 / 说明                                                      |
| ------------------ | ---------------------------------------------------------------- |
| ESP-IDF            | v5.4.4                                                           |
| Python（工具脚本） | 3.11 以上，依赖见 `tools/requirements.txt`                       |
| 宿主编译器（可选） | MSYS2 UCRT64 的 gcc/g++，用于跑渲染与历法的逐像素验证            |
| 串口               | 设备为 USB-Serial/JTAG，Windows 上通常是 COM5，烧录波特率 921600 |

```powershell
# 工具脚本的依赖
python -m venv tools/.venv
tools/.venv/Scripts/python.exe -m pip install -r tools/requirements.txt
```

## 构建与烧录

`firmware/` 是官方 Demo 的副本，只作驱动组件复用，不在本仓库内，需单独克隆：

```bash
git clone https://github.com/itopinion/zectrix-note4-epd-demo.git firmware
```

然后：

```powershell
# 构建（tools/idf.py 负责设置 IDF_TOOLS_PATH 与 PATH，直接调 idf.py 在本机会失败，原因见 docs/02）
tools/.venv/Scripts/python.exe tools/idf.py -C app build

# 烧录（设备先进下载模式：按住 BOOT 再按 RESET，随后松开 BOOT）
tools/.venv/Scripts/python.exe tools/flash_retry.py --port COM5 --baud 921600
```

**动手前先备份原厂 Flash**，那是唯一的退路：

```powershell
tools/.venv/Scripts/python.exe tools/flash_backup.py backup --port COM5 --baud 921600
```

## 配网

缺少 WiFi 或取数凭证时，设备开机进入配网窗口，屏幕上显示热点名与地址：

```powershell
# 串口写 WiFi
tools/.venv/Scripts/python.exe tools/provision.py --port COM5 --ssid MyWiFi --pass 12345678

# 或：手机连热点 Zectrix-Cal-XXXX，浏览器打开 http://192.168.4.1 填写
```

窗口按闲置计时，300 秒无操作退出。**想中途退出就短按 BOOT。**

## 取数配置

需要后端支持，才能正常展示班次信息。
本项目班次来自自建的华为端云一体化鸿蒙项目，通过AGC中转云函数获取，仓库内不含任何真实凭据。接自己的数据源需要准备：

| 配置                                         | 位置                                                                              |
| -------------------------------------------- | --------------------------------------------------------------------------------- |
| Client ID / Client Secret / apiKey / groupId | 设备 NVS，通过配网写入                                                            |
| 云函数地址与 productId                       | Kconfig：`CAL_AGC_PROXY_URL` / `CAL_AGC_PRODUCT_ID`，真实值写本地 `app/sdkconfig` |
| PC 侧探测脚本的同一组参数                    | `tools/agc_config.local.json`（不入库，模板见 `tools/agc_config.example.json`）   |

接口格式、请求示例与响应契约见 `docs/11`。

## 项目结构

```
zectrix_yike/
├── app/                    # 业务 IDF 工程
│   ├── main/               # 主流程：唤醒、自检、取数、校时、渲染、休眠
│   └── components/
│       ├── cal_almanac/    # 农历 / 节气 / 节日（纯 C99，零 IDF 依赖）
│       ├── cal_font/       # 点阵字库，25 组字号、1333 字形
│       ├── cal_render/     # 台历页绘制（零 IDF 依赖，可在 PC 上编）
│       ├── cal_net/        # WiFi、凭据存取、串口与网页配网
│       └── cal_sched/      # OAuth + 云函数取数、解析、缓存、校时
├── tools/                  # 备份 / 烧录 / 字库 / 渲染验证 / 配网脚本
├── docs/                   # 文档，入口 docs/00-项目总览.md
├── design/                 # 版式效果图
├── LICENSE
└── SECURITY.md
```

`cal_almanac`、`cal_render`、`cal_sched` 的解析部分**不引入任何 IDF 头**，同一份源码会在 PC 上用宿主编译器再编一遍，与 Python 参照实现做逐项或逐像素比对。这是本项目的主要验证手段，见 `tools/cal_render_test.py`、`tools/almanac_test.py`、`tools/sched_parse_test.py`。

## 硬件约束

| #   | 约束                                                                                                                            |
| --- | ------------------------------------------------------------------------------------------------------------------------------- |
| 1   | 刷任何固件前先完整备份 16 MiB 原厂 Flash，并做一次「备份 → 原样写回」空转测试                                                   |
| 2   | 不要开启 Secure Boot / Flash Encryption，那会烧 eFuse 且不可逆                                                                  |
| 3   | 业务固件用 `factory` 单分区、无 otadata，刷入会替换原厂分区表与 bootloader；救回走官方云平台的「本地升级」（写 `0x0` 全量镜像） |
| 4   | 必须用支持数据传输的 USB 线，直连不过 HUB，写入期间不要断电                                                                     |
| 5   | 日常省电只走深度休眠，不要调用原厂 `Shutdown()` —— 它是真断电，走了当天不再刷新                                                 |
| 6   | 深度休眠前必须用 `rtc_gpio_hold_en()` 锁存电池供电轨，否则整机断电、无法唤醒                                                    |

## 文档

入口是 [`docs/00-项目总览.md`](docs/00-项目总览.md)：硬件基线、唤醒流程、页面坐标、接口、按键、验证工具、红线。

| 文件                | 内容                               |
| ------------------- | ---------------------------------- |
| `docs/01`           | 首次刷机操作手册                   |
| `docs/02`           | Windows 环境搭建与踩坑记录         |
| `docs/03`           | 台历页规格与数据契约（版式权威）   |
| `docs/08`–`docs/10` | 每日刷新、按键、电量的设备验证记录 |
| `docs/11`           | AGC 接口调用说明（接口权威）       |
| `docs/12`           | WiFi 配网与连接                    |

过程文档（交接记录、排查快照、维护者自检清单）不随仓库发布。

## 许可

代码使用 [MIT 许可](LICENSE)。`firmware/` 内的官方 Demo 同样使用 MIT 许可，版权归其作者。

字形的点阵数据由 **Noto Sans SC**（SIL OFL 1.1）渲染生成，仓库不分发字体文件。

## 免责声明

刷机有风险。第三方固件会覆盖原厂固件与分区表，操作不当可能导致设备无法启动。请自负风险并保留 Flash 全量备份。本项目与 Zectrix 官方无关，不提供设备保修支持。

安全问题请按 [SECURITY.md](SECURITY.md) 的方式私下报告，不要开公开 issue。

## 致谢

- [itopinion/zectrix-note4-epd-demo](https://github.com/itopinion/zectrix-note4-epd-demo) —— 外设驱动与墨水屏参考实现
- [Noto Sans SC](https://fonts.google.com/noto) —— 字形来源
- Zectrix 官方 Wiki：<https://wiki.zectrix.com/zh/home>
