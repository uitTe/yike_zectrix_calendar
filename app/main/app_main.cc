// 每日台历：本地 RTC 零点唤醒，刷新离线历法后进入 deep sleep。

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

#include "cal_almanac.h"
#include "cal_font.h"
#include "cal_net.h"
#include "cal_render.h"
#include "cal_sched.h"
#include "cal_sched_net.h"
#include "calendar_button.h"
#include "calendar_clock.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "rtc_pcf8563.h"
#include "zectrix_board.h"
#include "zectrix_board_config.h"
#include "zectrix_canvas.h"
#include "zectrix_epd.h"
#include "zectrix_nfc.h"

#ifdef CALENDAR_RTC_CONSOLE
void CalendarRtcConsole(RtcPcf8563* rtc);
#endif

namespace {

constexpr char kTag[] = "calendar";
constexpr int64_t kButtonAwakeMs = 30000;
// 配网窗口按闲置计时：5 分钟内没有串口输入或网页请求才退出，硬上限 15 分钟。
constexpr int64_t kProvisionIdleMs = 300000;
// RTC 与服务器时间差异不超过这个秒数就不写。RTC 每天误差约 1.7 秒，
// 一天写一次足够，没必要每次唤醒都动一次 I2C。
constexpr long kClockWriteThresholdSeconds = 5;
// 上键两段式：按满 3 秒松开是重启刷新，一直按住到 8 秒才进配网。
// 两档之间留 5 秒缓冲——原来 6 秒与 3 秒只差 3 秒，按到 5 秒多就会撞进配网分支，
// 设备重启后又停在配网页，看起来像「按 3 秒没退出」。
constexpr int64_t kProvisionHoldMs = 8000;
// 配网期间按住 BOOT 多久算「要退出」。松手那条路径不限时长，这条只是照顾
// 「按住不放」的按法，两者任一命中都重启。
constexpr int64_t kProvisionBootHoldMs = 1500;
constexpr gpio_num_t kWakePins[] = {ZECTRIX_RTC_INT, ZECTRIX_BUTTON_OK};

ZectrixCanvas g_canvas;
zectrix_epd_handle_t g_epd = nullptr;
// 配网窗口是否打开。上键在配网里有两条路径（3 秒退出 / 8 秒重进），
// 判定的时间误差足以让退出失效，所以另给一条不受时长阈值影响的退出方式：
// 配网期间按 BOOT。
//
// 必须是 atomic：主任务在 EnsureProvisioned() 里写，ResetButtonTask 在自己的
// 循环里读。普通 bool 在 -O2 下会被编译器提升成寄存器常量，按键任务永远读到
// 初始的 false —— 表现就是「按了 BOOT 没反应」（2026-10-26 实测踩到）。
std::atomic<bool> g_provision_active{false};

// 长按 UP 重启后进配网：软件复位不清 RTC 内存，用魔数传递意图。
// 上电复位时这块内存内容不确定，因此必须校验魔数。
RTC_NOINIT_ATTR uint32_t g_provision_request;
constexpr uint32_t kProvisionMagic = 0x50524F56;  // "PROV"

// 配网期间按 BOOT 退出时置位，让重启后这一次跳过配网。
//
// 没有它的话「退出配网」是句空话：无凭据时 EnsureProvisioned() 每次启动都会进
// 配网，按 BOOT 只是重启再进一次，屏幕永远停在 SETUP（2026-10-26 实测确认）。
// 标志只消费一次，读到就清零，否则以后每次启动都会跳过配网。
RTC_NOINIT_ATTR uint32_t g_skip_provision;
constexpr uint32_t kSkipProvisionMagic = 0x534B4950;  // "SKIP"

void ResetButtonTask(void*) {
    CalendarResetButton button;
    bool holding = false;
    int64_t hold_start_ms = 0;
    bool boot_down = false;
    int64_t boot_down_ms = 0;
    for (;;) {
        const bool pressed = gpio_get_level(ZECTRIX_BUTTON_UP) == 0;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (pressed && !holding) {
            holding = true;
            hold_start_ms = now_ms;
        } else if (!pressed) {
            holding = false;
        }
        // 按住到 8 秒立刻进配网，不等松开；3 秒松开仍是原来的重启刷新。
        if (holding && now_ms - hold_start_ms >= kProvisionHoldMs) {
            ESP_LOGI(kTag, "UP held %lld ms: restart into provisioning",
                     static_cast<long long>(kProvisionHoldMs));
            g_provision_request = kProvisionMagic;
            fflush(stdout);
            esp_restart();
        }
        if (button.Update(pressed, now_ms)) {
            ESP_LOGI(kTag, "UP long press released: restart");
            fflush(stdout);
            esp_restart();
        }

        // 配网期间按 BOOT 退出。两条触发路径，覆盖实际按法：
        //   松手即退 —— 轻点一下就退出，不判按下时长
        //   按住 1.5 秒即退 —— 有人习惯按住不放（比如之前为了保持设备醒着），
        //                     只等松手的话这种按法永远退不出去
        // 退出要连带置 g_skip_provision：否则重启后因缺凭据又进配网，画面仍是
        // SETUP，用户看到的就是「按了没反应」。
        const bool boot_level = gpio_get_level(ZECTRIX_BUTTON_OK) == 0;
        if (boot_level && !boot_down) {
            boot_down = true;
            boot_down_ms = now_ms;
        } else if (boot_level && boot_down && g_provision_active.load() &&
                   now_ms - boot_down_ms >= kProvisionBootHoldMs) {
            ESP_LOGI(kTag, "BOOT held %lld ms: leave provisioning",
                     static_cast<long long>(now_ms - boot_down_ms));
            g_skip_provision = kSkipProvisionMagic;
            fflush(stdout);
            esp_restart();
        } else if (!boot_level && boot_down) {
            boot_down = false;
            if (g_provision_active.load()) {
                ESP_LOGI(kTag, "BOOT released after %lld ms: leave provisioning",
                         static_cast<long long>(now_ms - boot_down_ms));
                g_skip_provision = kSkipProvisionMagic;
                fflush(stdout);
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void WaitForUser() {
    ESP_LOGI(kTag, "BOOT wake: awake for 30 seconds, hold UP 3 seconds and release to restart");
    int64_t deadline = esp_timer_get_time() / 1000 + kButtonAwakeMs;
    while (esp_timer_get_time() / 1000 < deadline) {
        if (gpio_get_level(ZECTRIX_BUTTON_UP) == 0 ||
            gpio_get_level(ZECTRIX_BUTTON_OK) == 0) {
            deadline = esp_timer_get_time() / 1000 + kButtonAwakeMs;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

tm ReadCalendarTime(RtcPcf8563* rtc) {
    tm now = {};
    if (!rtc->GetTime(now) || !CalendarTimeValid(now)) {
        ESP_LOGE(kTag, "RTC 时间无效，需要校时");
        ESP_ERROR_CHECK(ESP_ERR_INVALID_STATE);
    }
    return now;
}

// ---------------------------------------------------------------- 存储与时钟
//
// NVS 是 WiFi 凭据与日历缓存的唯一存放位置。分区满或版本不符时按官方建议重建，
// 重建会清掉凭据与缓存，必须明确记录，不能让设备无声地失去配置。
void InitNvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "NVS 需要重建（%s），凭据与日历缓存将丢失", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// 把 PCF8563 的本地时间灌进系统时钟。
//
// mbedTLS 校验证书有效期依赖系统时间。未灌入时系统时间恒为 1970 年，
// HTTPS 握手会以「证书未生效」失败 —— 报错形似证书问题，实际是时间问题。
bool SeedSystemClockFromRtc(RtcPcf8563* rtc) {
    tm now = {};
    if (!rtc->GetTime(now) || !CalendarTimeValid(now)) {
        ESP_LOGW(kTag, "RTC 时间无效，系统时钟未设置");
        return false;
    }
    now.tm_isdst = -1;
    const time_t epoch = std::mktime(&now);
    if (epoch == static_cast<time_t>(-1)) {
        ESP_LOGW(kTag, "RTC 时间无法换算为时间戳");
        return false;
    }
    const timeval tv = {epoch, 0};
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGW(kTag, "settimeofday 失败");
        return false;
    }
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &now);
    ESP_LOGI(kTag, "system clock seeded from rtc: local %s", text);
    return true;
}

// ---------------------------------------------------------------- 历法自检
//
// 历法（农历 / 节气 / 节日）的正确性已经在 PC 上做了 20 万项黄金向量比对
// （tools/almanac_test.py，与 Python 参照实现逐字节一致）。板上这几条**不是**
// 重复那个测试，而是补 PC 端唯一测不了的部分：ESP32-S3 没有双精度 FPU，
// double 由软件模拟、libm 的 sin 也和新libc/Mingw 不同 —— 这里验证节气用的
// 天文法在板上算出的**日期**与 PC 一致（余量 5–10 分钟，日期判定对 libm 差异免疫）。
struct AlmanacCase {
    int y, m, d;
    const char* lunar;
    const char* weekday;
    const char* term;  // "" = 当天不显示节气（含「清明让位」）
    const char* fest0; // "" = 当天无节日
};

constexpr AlmanacCase kAlmanacCases[] = {
    {2026, 9, 23, "八月十三", "星期三", "秋分", ""},
    {2026, 4, 5, "二月十八", "星期日", "", "清明节"},   // 清明让位：只留节日
    {2031, 10, 1, "八月十五", "星期三", "", "中秋节"},  // 撞车：中秋 + 国庆
    {2037, 2, 14, "腊月三十", "星期六", "", "除夕"},    // 撞车：除夕 + 情人节
    {2027, 2, 6, "正月初一", "星期六", "", "春节"},
    {2028, 6, 23, "闰五月初一", "星期五", "", ""},       // 2028 闰五月，验证「闰」前缀
};

void RunAlmanacSelfCheck() {
    int bad = 0;
    for (const AlmanacCase& c : kAlmanacCases) {
        const cal_date d = {c.y, c.m, c.d};
        cal_day_info info;
        cal_day_info_get(&d, &info);
        const char* f0 = (info.fest_count > 0) ? info.festivals[0] : "";
        if (std::strcmp(info.lunar_cn, c.lunar) != 0 ||
            std::strcmp(info.weekday_cn, c.weekday) != 0 ||
            std::strcmp(info.term, c.term) != 0 ||
            std::strcmp(f0, c.fest0) != 0) {
            ++bad;
            ESP_LOGE(kTag,
                     "almanac MISMATCH %04d-%02d-%02d got [%s|%s|%s|%s] "
                     "want [%s|%s|%s|%s]",
                     c.y, c.m, c.d, info.lunar_cn, info.weekday_cn, info.term, f0,
                     c.lunar, c.weekday, c.term, c.fest0);
        }
    }
    ESP_LOGI(kTag, "almanac self-check: %u cases, %d mismatch",
             static_cast<unsigned>(sizeof(kAlmanacCases) / sizeof(kAlmanacCases[0])),
             bad);
}

// 读板载 RTC，打印「今天」的全部离线信息（后续台历页要用同一份数据）
void LogTodayAlmanac(ZectrixBoard* board) {
    if (board->rtc() == nullptr) {
        ESP_LOGW(kTag, "no rtc instance");
        return;
    }
    tm t = {};
    if (!board->rtc()->GetTime(t)) {
        ESP_LOGW(kTag, "rtc read failed");
        return;
    }
    const cal_date d = {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday};
    cal_day_info info;
    cal_day_info_get(&d, &info);

    char fest[64] = "";
    for (int i = 0; i < info.fest_count; ++i) {
        if (i > 0) std::strncat(fest, "+", sizeof(fest) - std::strlen(fest) - 1);
        std::strncat(fest, info.festivals[i], sizeof(fest) - std::strlen(fest) - 1);
    }
    ESP_LOGI(kTag, "today %s %s  农历 %s  节气 %s  节日 %s  星期idx=%d",
             info.date_cn, info.weekday_cn, info.lunar_cn,
             info.term[0] ? info.term : "-", fest[0] ? fest : "-", info.weekday);
}

// ---------------------------------------------------------------- 字库自检
//
// 字号清单由 tools/pt_census.py 普查 1900–2099 全部真实内容定出（25 组 / 1333 字形），
// 逐像素一致性已在 PC 上验过（tools/font_verify.py：逐字 3939 例 + 整串 99 例全零差异）。
// 板上这一项做三件事：
//   ① 确认点阵在 Flash 里读得出来、字形盒自洽；
//   ② 实证「候选集内降序取最大满足 pt」这条**替代二分**的选字号链路；
//   ③ **让字库真正被引用** —— 当前固件只有锁存探针、还没用到字体绘制，
//      链接器 --gc-sections 会把整份点阵丢出二进制（实测少了 ~57 KB）。
struct PickCase {
    const char* text;
    int target_h;
    int want_pt;
};

// 期望值取自 tools/font_verify.py 的实测输出（PC 侧 font_for_height 的结果）
constexpr PickCase kPickCases[] = {
    {"2026年9月23日", 27, 28},
    {"八月十三", 21, 21},
    {"八月十五", 18, 18},
    {"腊月初八", 18, 17},
    {"七月十五", 15, 15},
    {"获取失败", 30, 30},
    {"获取失败", 18, 17},
    {"上午08:00-12:00", 30, 31},
    {"下午14:00-18:00", 30, 32},
    {"夜间20:00-24:00", 28, 28},
};

// 选字号 = cal_font_pick_pt()（组件公开 API，与 PC 侧 font_for_height 等价）。
//
// ⚠️ 曾经在这里自己实现过一版「按 kCalFontGroups 顺序取第一个满足 height<=target_h 的」，
// 并注释说组是**降序**排列 —— 那是错的，实测数据是 pt **升序**（14 15 … 72），于是取到的是
// **最小** pt，整页字号全线塌到 pt14。2026-10-14 被本文件的自检抓到（10 例 9 mismatch）。
// 现在的实现显式取「最大满足 pt」且要求整串无缺字，与数据排列顺序解耦。

// 班次名**已知形态** —— 必须与 tools/pt_census.py::SHIFT_KNOWN_FORMS 保持一致。
//
// 这是全页唯一「内容来自第三方数据源、**不可穷举**」的区域：2026-10-14 就因为漏了
// 「值班」二字，字库里查不到字形，屏幕上直接画成空白，而所有自动化测试都是绿的
// （PC 侧比对两边都用 Pillow 渲染，谁都没查清单）。故开机自检逐个形态 × pt20–28
// 查一遍字形是否齐 —— 换了字库没重跑清单，这里必须报警。
constexpr const char* kShiftForms[] = {
    "休",        "休息",        "9DP1",         "9NP1",
    "9DP(绿)",   "9DP1(绿)",    "9NP1(绿)",     "9DP0(验车)",
    "9DP1(值班)", "9DP0(值班)",
};

void RunFontSelfCheck() {
    size_t glyph_total = 0;
    size_t ink_hits = 0;
    size_t empty = 0;
    for (size_t i = 0; i < kCalFontGroupCount; ++i) {
        const CalFontGroup& g = kCalFontGroups[i];
        glyph_total += g.count;
        for (uint16_t k = 0; k < g.count; ++k) {
            const CalGlyph& gl = g.glyphs[k];
            // 真扫一遍点阵：glyphs[].offset 只是数字索引，不碰位图的话
            // 链接器不会保留 bitmap 池
            if (gl.ink_w > 0 && gl.ink_h > 0 &&
                cal_glyph_pixel(&g, &gl, gl.ink_h / 2, gl.ink_w / 2)) {
                ++ink_hits;
            }
            if (gl.bbox_w == 0 && gl.bbox_h == 0 && gl.ink_w == 0 && gl.ink_h == 0) {
                ++empty;
            }
        }
    }
    ESP_LOGI(kTag, "font self-check: %u groups, %u glyphs, %u ink hits, %u empty",
             static_cast<unsigned>(kCalFontGroupCount),
             static_cast<unsigned>(glyph_total),
             static_cast<unsigned>(ink_hits),
             static_cast<unsigned>(empty));

    // 布局链路抽查：顶栏日期的真实锚点与字号（期望值来自 PC 侧实测）
    CalTextMetrics tm = {};
    if (cal_text_layout(28, "2026年9月23日", CAL_ALIGN_LEFT, 16, 30, &tm)) {
        ESP_LOGI(kTag, "font layout probe: 2026年9月23日 @pt28 -> %dx%d (want 203x27)",
                 tm.width, tm.height);
    } else {
        ESP_LOGE(kTag, "font layout probe FAILED");
    }

    // 选字号链路：候选集降序查找 vs PC 侧二分
    int bad = 0;
    for (const PickCase& c : kPickCases) {
        const int got = cal_font_pick_pt(c.text, c.target_h);
        if (got != c.want_pt) {
            ++bad;
            ESP_LOGE(kTag, "pick MISMATCH %s h=%d got pt%d want pt%d",
                     c.text, c.target_h, got, c.want_pt);
        }
    }
    ESP_LOGI(kTag, "font pick check: %u cases, %d mismatch",
             static_cast<unsigned>(sizeof(kPickCases) / sizeof(kPickCases[0])), bad);

    // 班次名词汇守卫 —— 见 kShiftForms 上方说明。
    // 用 cal_font_missing_count 查表：它不终止进程，只报缺字个数 + 首个缺失码点。
    constexpr int kShiftPtMin = 20;
    constexpr int kShiftPtMax = 28;
    constexpr size_t kShiftFormCount = sizeof(kShiftForms) / sizeof(kShiftForms[0]);
    int vocab_bad = 0;
    for (int pt = kShiftPtMin; pt <= kShiftPtMax; ++pt) {
        for (size_t i = 0; i < kShiftFormCount; ++i) {
            uint32_t first = 0;
            const int missing = cal_font_missing_count(pt, kShiftForms[i], &first);
            if (missing < 0) {
                ESP_LOGE(kTag, "font shift vocab: pt%d 整个字号组不在字库内", pt);
                ++vocab_bad;
            } else if (missing > 0) {
                ESP_LOGE(kTag, "font shift vocab MISS pt%d %s -> %d char(s), 首个 U+%04X",
                         pt, kShiftForms[i], missing,
                         static_cast<unsigned>(first));
                ++vocab_bad;
            }
        }
    }
    ESP_LOGI(kTag, "font shift vocab: %u forms x pt%d-%d = %u checks, %d bad",
             static_cast<unsigned>(kShiftFormCount), kShiftPtMin, kShiftPtMax,
             static_cast<unsigned>(kShiftFormCount *
                                   (kShiftPtMax - kShiftPtMin + 1)),
             vocab_bad);

    // 新形态的板端实测宽度（期望值与 PC 侧一致：9DP1(值班) @pt28 → 150x30）
    CalTextMetrics shift_tm = {};
    if (cal_text_layout(28, "9DP1(值班)", CAL_ALIGN_LEFT, 0, 0, &shift_tm)) {
        if (shift_tm.width != 150 || shift_tm.height != 30 || shift_tm.missing != 0) {
            ESP_LOGE(kTag, "font shift probe MISMATCH %dx%d missing=%d (want 150x30 0)",
                     shift_tm.width, shift_tm.height, shift_tm.missing);
        } else {
            ESP_LOGI(kTag, "font shift probe: 9DP1(值班) @pt28 -> %dx%d OK",
                     shift_tm.width, shift_tm.height);
        }
    } else {
        ESP_LOGE(kTag, "font shift probe FAILED: pt28 字库里没有「值班」");
    }

    // B 方案（缺字就地跳过）的**板端实测** —— 「早」未登记进任何字号组，用它验证
    // 绘制路径是「跳过」而不是「终止进程」。判据：宽度只多出半字宽，高度不变。
    // 宿主侧跑的是同一份源码（tools/font_missing_test.py），板上这一条确认的是
    // **实际烧进设备的这份二进制**确实没有 abort 分支 —— 缺字让设备当机的代价
    // 是当天不再刷新，而它一天只醒一次。
    CalTextMetrics clean = {};
    CalTextMetrics dirty = {};
    const bool skip_ok =
        cal_text_layout(28, "9DP1(值班)", CAL_ALIGN_LEFT, 0, 0, &clean) &&
        cal_text_layout(28, "9DP1(值班早)", CAL_ALIGN_LEFT, 0, 0, &dirty) &&
        clean.missing == 0 && dirty.missing == 1 &&
        dirty.height == clean.height &&
        dirty.width == clean.width + CAL_MISSING_ADVANCE_PX(28);
    if (skip_ok) {
        ESP_LOGI(kTag,
                 "font skip probe: 9DP1(值班早) @pt28 -> %dx%d missing=%d "
                 "(干净串 %dx%d，差 %d = pt28/2) OK",
                 dirty.width, dirty.height, dirty.missing, clean.width, clean.height,
                 CAL_MISSING_ADVANCE_PX(28));
    } else {
        ESP_LOGE(kTag, "font skip probe MISMATCH clean=%dx%d m=%d dirty=%dx%d m=%d",
                 clean.width, clean.height, clean.missing, dirty.width, dirty.height,
                 dirty.missing);
    }
}

// ---------------------------------------------------------------- 配网
//
// 设备只有 OK 与 UP 两个按键、一块单色屏，逐字符输入 8 位以上的 WiFi 密码不成立，
// 因此凭据只能从外部写入。串口用 USB Serial/JTAG，接上电脑即可，不需要拆机。

// ZectrixCanvas 只有 ASCII 8x16 点阵，提示文字统一用英文。
// 顶部固定写 SETUP，把热点名与地址放在下面，避免只看到热点名时误判。
// 末行提示网页还能填取数凭证 —— 那两个长串在串口上一行打不全，也不该出现在屏幕上。
void ShowProvisionScreen(const char* line1, const char* line2) {
    if (g_epd == nullptr) {
        return;
    }
    g_canvas.Clear(true);
    g_canvas.Text(20, 24, "SETUP", 2);
    g_canvas.Text(20, 76, line1, 1);
    g_canvas.Text(20, 108, line2, 1);
    g_canvas.Text(20, 170, "USB-Serial/JTAG 115200", 1);
    g_canvas.Text(20, 210, "web: WiFi + data credentials", 1);
    ESP_ERROR_CHECK(zectrix_epd_power_on(g_epd));
    ESP_ERROR_CHECK(zectrix_epd_refresh_full_1bpp(g_epd, g_canvas.data(),
                                                 g_canvas.size()));
    ESP_ERROR_CHECK(zectrix_epd_power_off(g_epd));
}

// 没有凭据时进入配网窗口，长按上键主动重启也会进入。超时按未配网继续，台历页照常刷新。
//
// 两类凭据都必须齐：WiFi 是通路，取数凭证（Client ID / Secret / apiKey / groupId）
// 决定能不能换到排班。缺任一类都进配网 —— 缺 WiFi 时只能重连，缺凭证时只补凭证，
// 配网页两块可分别留空。
void EnsureProvisioned(zectrix_epd_handle_t epd) {
    const bool requested = g_provision_request == kProvisionMagic;
    g_provision_request = 0;
    // 上次按 BOOT 退出配网留下的意图：这一次直接跳过，只跑离线历法。
    // 手动触发进配网的优先级更高，所以 requested 时忽略它。
    const bool skip_once = g_skip_provision == kSkipProvisionMagic;
    g_skip_provision = 0;
    CalWifiCredentials cred;
    CalApiCredentials api;
    const bool has_wifi = CalWifiLoad(&cred);
    const bool has_api = CalApiCredLoad(&api);
    if (!requested && has_wifi && has_api) {
        return;
    }
    if (skip_once && !requested) {
        ESP_LOGW(kTag, "跳过配网（上次按 BOOT 退出时请求），本次只用离线历法");
        return;
    }
    g_epd = epd;
    g_provision_active.store(true);
    if (requested) {
        ESP_LOGW(kTag, "进入配网模式（手动触发）");
    } else if (!has_wifi && !has_api) {
        ESP_LOGW(kTag, "进入配网模式（WiFi 与取数凭证都没有）");
    } else if (!has_wifi) {
        ESP_LOGW(kTag, "进入配网模式（缺 WiFi 凭据）");
    } else {
        ESP_LOGW(kTag, "进入配网模式（缺取数凭证）");
    }
    if (!CalProvisionRun(kProvisionIdleMs, ShowProvisionScreen)) {
        ESP_LOGW(kTag, "配网未完成，按未配网继续运行");
    }
    g_provision_active.store(false);
    g_epd = nullptr;
}

// ---------------------------------------------------------------- 台历页渲染
//
// 渲染器是 `tools/calendar_A_final.py` 的固件侧对等实现（组件 cal_render）。
// 版式真源在 PC 侧，改版式先改 Python 再同步过来。

void RenderPage(ZectrixCanvas* canvas, const cal_day_info* info, const char* shift,
                const char* am, const char* pm, bool blank_schedule = false,
                const CalBatteryInput* battery = nullptr,
                const CalWifiInput* wifi = nullptr) {
    CalPageInput in = {};
    in.info = info;
    in.shift = shift;
    in.am = am;
    in.pm = pm;
    in.blank_schedule = blank_schedule;
    in.battery = battery;
    in.wifi = wifi;
    CalFrame frame = {canvas->data(), ZectrixCanvas::kWidth, ZectrixCanvas::kHeight};
    cal_render_page(&frame, &in);
}

// 用服务器时间校准 RTC（docs/03 §6.1：零额外请求，直接取 HTTPS 响应头的 Date）。
// 返回 true 表示确实写了，调用方必须重读时间 —— 跨零点时日期会变。
bool ApplyServerTime(RtcPcf8563* rtc, const CalSchedUpdateResult& update) {
    if (rtc == nullptr || !update.has_server_time) {
        return false;
    }
    tm target = {};
    target.tm_year = update.year - 1900;
    target.tm_mon = update.month - 1;
    target.tm_mday = update.day;
    target.tm_hour = update.hour;
    target.tm_min = update.minute;
    target.tm_sec = update.second;
    target.tm_isdst = -1;
    const time_t target_epoch = std::mktime(&target);
    if (target_epoch == static_cast<time_t>(-1)) {
        ESP_LOGW(kTag, "服务器时间无法换算为时间戳");
        return false;
    }
    tm current = {};
    if (!rtc->GetTime(current)) {
        ESP_LOGW(kTag, "校时前读 RTC 失败");
        return false;
    }
    current.tm_isdst = -1;
    const time_t current_epoch = std::mktime(&current);
    const long drift = std::labs(static_cast<long>(target_epoch - current_epoch));
    if (drift <= kClockWriteThresholdSeconds) {
        ESP_LOGI(kTag, "RTC 与服务器相差 %ld 秒，不写", drift);
        return false;
    }
    if (!rtc->SetTime(target)) {
        ESP_LOGE(kTag, "写入 RTC 失败");
        return false;
    }
    const timeval tv = {target_epoch, 0};
    settimeofday(&tv, nullptr);
    ESP_LOGW(kTag, "RTC 已校准，偏差 %ld 秒", drift);
    return true;
}

// 页面用的排班。三态（docs/03 §2.2）：
//   blank=true                     → 这天没有排班数据，主区留白、底栏不画块
//   blank=false 且三项非空         → 正常显示班次与两段时段
//   blank=false 且三项全空         → 拉取失败，「获取失败」
struct PageSchedule {
    CalSchedDay day = {};
    bool blank = true;
};

PageSchedule ResolveSchedule(const CalSchedCache& cache, const tm& day_time) {
    PageSchedule out;
    CalSchedDay day;
    if (!cal_sched_cache_find(&cache, day_time.tm_year + 1900, day_time.tm_mon + 1,
                              day_time.tm_mday, &day)) {
        // 缓存里没有这一天：本次没取到，本地也没有历史数据。没有排班信息可用，
        // 按「获取失败」画 —— 画成全天空白会被当成「今天休息」，那是另一种错误。
        out.blank = false;
        return out;
    }
    out.day = day;
    out.blank = !day.has_schedule;
    return out;
}

// 取数、校时、绘页。返回实际出页用的时间 —— 校时可能把日期改到次日，
// 调用方要用这个值判断「刷出来的到底是不是今天」，否则会立刻再刷一轮。
tm RenderTodayPage(const tm& displayed, ZectrixBoard* board) {
    // 充电引脚需要连续采样，覆盖状态判定的稳定时间与交替窗口。
    ZectrixPowerSnapshot power = {};
    for (int i = 0; i < 17; ++i) {
        power = board->ReadPowerSnapshot();
        if (i < 16) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ESP_ERROR_CHECK(power.battery_valid ? ESP_OK : ESP_ERR_INVALID_STATE);
    CalBatteryInput battery = {power.battery_percent, CAL_BATTERY_NORMAL};
    if (power.charge.fault) {
        battery.state = CAL_BATTERY_FAULT;
    } else if (power.charge.no_battery) {
        battery.state = CAL_BATTERY_ABSENT;
    } else if (power.charge.full) {
        battery.state = CAL_BATTERY_FULL;
    } else if (power.charge.charging) {
        battery.state = CAL_BATTERY_CHARGING;
    }

    // 取数：连 WiFi → 换 token → 调云函数 → 写缓存，失败按 1 分钟 ×3 重试。
    // 凭证不全就跳过，页面照常出（班次区按「获取失败」画）。
    // 必须值初始化：缺凭据时不进 CalSchedUpdate()，若 update.cache.count 留成栈上
    // 垃圾值，ResolveSchedule() 会拿它当循环上界越界读，直接 panic
    // （2026-10-26 实测：Cache disabled but cached memory region accessed / EXCVADDR 0）。
    CalSchedUpdateResult update = {};
    CalWifiCredentials wifi_cred;
    CalApiCredentials api_cred;
    if (CalWifiLoad(&wifi_cred) && CalApiCredLoad(&api_cred)) {
        CalSchedUpdate(wifi_cred, api_cred, &update);
    } else {
        ESP_LOGW(kTag, "缺 WiFi 或取数凭证，本次不取数");
    }

    // 校时后日期可能变（RTC 慢到前一天 23:5x 又恰好当天唤醒），必须重读时间再出页。
    tm page_time = displayed;
    if (ApplyServerTime(board->rtc(), update)) {
        page_time = ReadCalendarTime(board->rtc());
        if (!CalendarSameDate(page_time, displayed)) {
            ESP_LOGW(kTag, "校时后日期变化，按新日期出页");
        }
    }

    const cal_date today = {page_time.tm_year + 1900, page_time.tm_mon + 1,
                            page_time.tm_mday};
    cal_day_info info;
    cal_day_info_get(&today, &info);
    const PageSchedule sched = ResolveSchedule(update.cache, page_time);
    const CalWifiInput wifi = {update.wifi_connected, update.rssi};
    RenderPage(&g_canvas, &info, sched.day.shift, sched.day.am, sched.day.pm, sched.blank,
               &battery, &wifi);
    ESP_LOGI(kTag, "battery %u mV raw=%u%% state=%d",
             power.battery_mv, power.battery_percent, static_cast<int>(battery.state));

    char fest[64] = "";
    for (int i = 0; i < info.fest_count; ++i) {
        if (i > 0) std::strncat(fest, "+", sizeof(fest) - std::strlen(fest) - 1);
        std::strncat(fest, info.festivals[i], sizeof(fest) - std::strlen(fest) - 1);
    }
    if (sched.blank) {
        ESP_LOGI(kTag, "page %s %s  农历 %s  节气 %s  节日 %s  班次：该天无排班",
                 info.date_cn, info.weekday_cn, info.lunar_cn,
                 info.term[0] ? info.term : "-", fest[0] ? fest : "-");
    } else if (sched.day.shift[0] == '\0' && sched.day.am[0] == '\0' &&
               sched.day.pm[0] == '\0') {
        ESP_LOGI(kTag, "page %s %s  农历 %s  节气 %s  节日 %s  班次：无可用数据",
                 info.date_cn, info.weekday_cn, info.lunar_cn,
                 info.term[0] ? info.term : "-", fest[0] ? fest : "-");
    } else {
        ESP_LOGI(kTag, "page %s %s  农历 %s  节气 %s  节日 %s  班次 %s [%s] [%s]",
                 info.date_cn, info.weekday_cn, info.lunar_cn,
                 info.term[0] ? info.term : "-", fest[0] ? fest : "-", sched.day.shift,
                 sched.day.am, sched.day.pm);
    }
    return page_time;
}

// 板端渲染自检 —— 与 PC 侧的**逐像素**比对（tools/cal_render_test.py）互补：
//   · PC 侧证的是「同一份源码 + 同一份字库 → 同一张图」（17 用例全帧零差异）；
//   · 板上这一项证的是**烧进设备的这份二进制**确实产出同一张图 —— 历法里的节气要用
//     double（S3 **没有双精度 FPU**，double 是软件模拟、libm 与 PC 不同源），
//     字库也可能因为别的原因与 PC 不一致。
//
// 判据 = **黑像素总数**（0 的位数）。它一变化就说明有东西被移动、缩小或漏画了，
// 比逐个探点灵敏得多；期望值取自 PC 权威渲染器的同一帧（真源见 CASE 注释）。
struct RenderCase {
    int y, m, d;
    const char* shift;
    const char* am;
    const char* pm;
    int black_pixels;  // 期望黑像素数（PC 侧同一帧统计）
    bool blank_schedule = false;
};

constexpr RenderCase kRenderCases[] = {
    // 秋分 / 时段两行 / 无节日 —— 定稿基准帧
    {2026, 9, 23, "9DP1", "08:00-12:00", "14:00-18:00", 16475},
    // 休息日：主区换「休息」pt72，底栏仍画班次反白块
    {2026, 6, 15, "休", "", "", 14968},
    // 拉取失败：主区「获取失败」pt30，底栏不画班次块
    {2026, 8, 8, "", "", "", 9366},
    // 同日两个 3 字节日（最坏签行）：退到 pad9/gap13 —— 签行几何的哨兵
    {2031, 10, 1, "9DP1", "08:00-12:00", "14:00-18:00", 18460},
    // 全角括号 → 归一化 + 班次名超宽降 pt + 块心平移
    {2026, 9, 27, "9DP0（验车）", "08:00-12:00", "14:00-18:00", 19001},
    // 农历侧 2 字 + 公历侧 3 字节日并存
    {2037, 2, 14, "9DP1", "08:00-12:00", "14:00-18:00", 18410},
    {2026, 9, 25, "", "", "", 9358, true},
    {2026, 10, 1, "", "", "", 9434, true},
    {2027, 1, 1, "", "", "", 8787, true},
};

void RunRenderSelfCheck() {
    // 版式常量守卫：与 PC 侧同名常量对账（改一边忘了另一边，这里立刻报）
    const CalLayout* L = cal_layout();
    const bool layout_ok = L != nullptr && L->width == 400 && L->height == 300 &&
                           L->div_x == 232 && L->week_cx == 316 && L->week_pt == 44 &&
                           L->right_x == 384 && L->fest_pt == 18 && L->fest_box_h == 26 &&
                           L->term_cx == 367 && L->term_pt == 31 && L->shift_cx == 320 &&
                           L->shift_pt == 28 && L->shift_box_h == 52;
    if (!layout_ok) {
        ESP_LOGE(kTag, "render layout MISMATCH: 版式常量与 PC 侧不一致");
    }

    int bad = 0;
    for (const RenderCase& c : kRenderCases) {
        const cal_date d = {c.y, c.m, c.d};
        cal_day_info info;
        cal_day_info_get(&d, &info);
        RenderPage(&g_canvas, &info, c.shift, c.am, c.pm, c.blank_schedule);

        int black = 0;
        const uint8_t* p = g_canvas.data();
        for (size_t i = 0; i < g_canvas.size(); ++i) {
            black += 8 - __builtin_popcount(static_cast<unsigned>(p[i]));
        }
        if (black != c.black_pixels) {
            ++bad;
            ESP_LOGE(kTag, "render MISMATCH %04d-%02d-%02d [%s] 黑像素 %d，期望 %d",
                     c.y, c.m, c.d, c.shift, black, c.black_pixels);
        }
    }
    ESP_LOGI(kTag, "render self-check: layout %s, %u cases, %d mismatch",
             layout_ok ? "OK" : "BAD",
             static_cast<unsigned>(sizeof(kRenderCases) / sizeof(kRenderCases[0])), bad);
}

// ---------------------------------------------------------------- 取数自检
//
// 取数是全页唯一「内容来自第三方、格式会悄悄变」的环节，而且只有联网才走得到 ——
// 烧进设备后不一定每次都有网。这里用一份结构同实测响应的合成样本，在板上把
// 解析、缓存编解码、Date 头换算各验一遍，不碰网络。样本里不含真实排班数据。
//
// 与 PC 侧 `tools/sched_parse_test.py` 的分工：那边测同一份源码的完整用例（含 6 种
// 违约样本）；这里测的是**烧进设备这份二进制**确实带上了同样的行为 —— 缺字段被
// 判成「没排班」而不是「获取失败」，缓存往返不丢字，都是页面级的正确性。
//
// 样本要跟着 `docs/11` §5.4 走：服务端改了字段，这里先报警比等页面画错更早。
constexpr char kSchedSample[] =
    "{\"code\":200,\"message\":\"查询成功\",\"data\":{"
    "\"groupId\":\"grp_example_0000000000000\",\"startDate\":\"2026-10-22\","
    "\"endDate\":\"2026-11-04\",\"days\":["
    "{\"date\":\"2026-10-22\",\"shiftName\":\"休\",\"amStart\":\"00:00\",\"amEnd\":\"00:00\","
    "\"pmStart\":\"00:00\",\"pmEnd\":\"00:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-10-23\",\"shiftName\":\"A1\",\"amStart\":\"12:30\",\"amEnd\":\"17:00\","
    "\"pmStart\":\"18:00\",\"pmEnd\":\"21:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-10-24\",\"shiftName\":\"C2（验车）\",\"amStart\":\"09:00\","
    "\"amEnd\":\"12:00\",\"pmStart\":\"13:15\",\"pmEnd\":\"18:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-10-25\",\"shiftName\":\"休\",\"amStart\":\"00:00\",\"amEnd\":\"00:00\","
    "\"pmStart\":\"00:00\",\"pmEnd\":\"00:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-10-26\",\"shiftName\":\"B1\",\"amStart\":\"09:30\",\"amEnd\":\"13:15\","
    "\"pmStart\":\"17:00\",\"pmEnd\":\"21:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-10-27\",\"shiftName\":\"休\",\"amStart\":\"00:00\",\"amEnd\":\"00:00\","
    "\"pmStart\":\"00:00\",\"pmEnd\":\"00:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-10-28\",\"shiftName\":\"A2\",\"amStart\":\"09:00\",\"amEnd\":\"13:00\","
    "\"pmStart\":\"14:15\",\"pmEnd\":\"18:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-10-29\",\"shiftName\":\"A2\",\"amStart\":\"09:00\",\"amEnd\":\"13:00\","
    "\"pmStart\":\"14:15\",\"pmEnd\":\"18:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-10-30\",\"shiftName\":\"A2\",\"amStart\":\"09:00\",\"amEnd\":\"13:00\","
    "\"pmStart\":\"14:15\",\"pmEnd\":\"18:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-10-31\",\"shiftName\":\"休\",\"amStart\":\"00:00\",\"amEnd\":\"00:00\","
    "\"pmStart\":\"00:00\",\"pmEnd\":\"00:00\",\"remark\":\"\"},"
    "{\"date\":\"2026-11-01\",\"remark\":\"\"},"
    "{\"date\":\"2026-11-02\",\"remark\":\"\"},"
    "{\"date\":\"2026-11-03\",\"remark\":\"\"},"
    "{\"date\":\"2026-11-04\",\"remark\":\"\"}]}}";

void RunSchedSelfCheck() {
    struct ExpectDay {
        int index;
        const char* shift;
        const char* am;
        const char* pm;
        bool has_schedule;
    };
    // 期望值取自合成样例（结构同 docs/11 §5.4 的实测响应）：10-22 是休班
    // （时段必须被忽略）、10-24 带全角括号、11-01 起服务端省略 shiftName。
    constexpr ExpectDay kExpected[] = {
        {0, "休", "", "", true},
        {1, "A1", "12:30-17:00", "18:00-21:00", true},
        {2, "C2（验车）", "09:00-12:00", "13:15-18:00", true},
        {4, "B1", "09:30-13:15", "17:00-21:00", true},
        {10, "", "", "", false},
        {13, "", "", "", false},
    };
    // 这段自检只在启动时跑一次，且要放 1.1 KB 的缓存与 1.1 KB 的序列化缓冲 ——
    // 放静态区，别拿主任务那 8 KB 栈去扛。
    static CalSchedPayload payload;
    static CalSchedCache cache;
    static CalSchedCache restored;
    static uint8_t blob[CAL_SCHED_CACHE_BYTES];

    int bad = 0;
    if (!cal_sched_parse(kSchedSample, sizeof(kSchedSample) - 1, &payload)) {
        ESP_LOGE(kTag, "sched self-check: 样本解析失败 %s", cal_sched_last_error());
        return;
    }
    if (payload.code != 200 || payload.count != 14) {
        ++bad;
        ESP_LOGE(kTag, "sched self-check: code=%d count=%d（期望 200 / 14）", payload.code,
                 payload.count);
    }
    for (const ExpectDay& expect : kExpected) {
        if (expect.index >= payload.count) {
            ++bad;
            ESP_LOGE(kTag, "sched self-check: 样本只有 %d 条，缺第 %d 条", payload.count,
                     expect.index);
            continue;
        }
        const CalSchedDay& day = payload.days[expect.index];
        if (day.has_schedule != expect.has_schedule ||
            std::strcmp(day.shift, expect.shift) != 0 ||
            std::strcmp(day.am, expect.am) != 0 ||
            std::strcmp(day.pm, expect.pm) != 0) {
            ++bad;
            ESP_LOGE(kTag, "sched self-check MISMATCH 第 %d 条 got [%d|%s|%s|%s]",
                     expect.index, day.has_schedule ? 1 : 0, day.shift, day.am, day.pm);
        }
    }

    // 缓存往返：格式坏了页面会整天没有班次，而这种错误在设备上没有任何提示。
    cal_sched_cache_clear(&cache);
    cache.fetched_epoch = 1792650056;
    cache.count = payload.count;
    for (int i = 0; i < payload.count; ++i) {
        cache.days[i] = payload.days[i];
    }
    const size_t packed = cal_sched_cache_pack(&cache, blob, sizeof(blob));
    CalSchedDay found;
    if (packed != CAL_SCHED_CACHE_BYTES ||
        !cal_sched_cache_unpack(blob, packed, &restored) ||
        !cal_sched_cache_find(&restored, 2026, 10, 26, &found) ||
        std::strcmp(found.shift, "B1") != 0 || std::strcmp(found.am, "09:30-13:15") != 0) {
        ++bad;
        ESP_LOGE(kTag, "sched self-check: 缓存往返失败（packed=%u）",
                 static_cast<unsigned>(packed));
    }

    // Date 头换算：校时算错就是整页日期、农历、节气全错位。
    int64_t epoch = 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!cal_sched_parse_http_date("Thu, 22 Oct 2026 09:40:56 GMT", &epoch) ||
        !cal_sched_beijing_time(epoch, &year, &month, &day, &hour, &minute, &second) ||
        year != 2026 || month != 10 || day != 22 || hour != 17 || minute != 40 ||
        second != 56) {
        ++bad;
        ESP_LOGE(kTag,
                 "sched self-check: Date 头换算错误 %04d-%02d-%02d %02d:%02d:%02d"
                 "（期望 2026-10-22 17:40:56）",
                 year, month, day, hour, minute, second);
    }

    // 令牌复用判据：余量要吃掉边缘，时钟回退时不能判死（交给 401 兜底重换）。
    const int64_t now = cal_sched_epoch_from_date(2026, 10, 26) + 11 * 3600;
    if (!cal_sched_token_usable(now + 7200, now, 3600) ||
        cal_sched_token_usable(now + 1800, now, 3600) ||
        cal_sched_token_usable(now + 3600, now, 3600) ||
        cal_sched_token_usable(0, now, 3600) ||
        !cal_sched_token_usable(now + 7200, 0, 3600)) {
        ++bad;
        ESP_LOGE(kTag, "sched self-check: 令牌有效期判据错误（余量或时钟回退边界）");
    }

    ESP_LOGI(kTag, "sched self-check: %u 条样本、缓存往返、Date 头、令牌有效期，%d mismatch",
             static_cast<unsigned>(sizeof(kExpected) / sizeof(kExpected[0])), bad);
}

[[noreturn]] void SleepUntilNextWake(ZectrixBoard* board, const tm& next) {
#ifdef CALENDAR_RTC_CONSOLE
    // 设备验证时保留可操作窗口，确认闹钟与页面后才进入实际休眠。
    ESP_LOGI(kTag, "armed until %04d-%02d-%02d 00:00:00",
             next.tm_year + 1900, next.tm_mon + 1, next.tm_mday);
    CalendarRtcConsole(board->rtc());
#endif
    // BOOT 松开并稳定后才休眠，避免同一次按下反复唤醒。
    int64_t released_at = esp_timer_get_time() / 1000;
    while (esp_timer_get_time() / 1000 - released_at < 60) {
        if (gpio_get_level(ZECTRIX_BUTTON_OK) == 0 ||
            gpio_get_level(ZECTRIX_BUTTON_UP) == 0) {
            released_at = esp_timer_get_time() / 1000;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // 射频先关，再断其余外设，最后才锁存电池供电。
    CalWifiShutdown();
    board->SetAudioPower(false);
    board->SetPowerLed(false);
    if (board->nfc() != nullptr) {
        board->nfc()->PowerOff();
    }

    for (gpio_num_t pin : kWakePins) {
        ESP_ERROR_CHECK(rtc_gpio_init(pin));
        ESP_ERROR_CHECK(rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(pin));
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(pin));
    }
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
        (1ULL << ZECTRIX_RTC_INT) | (1ULL << ZECTRIX_BUTTON_OK), ESP_EXT1_WAKEUP_ANY_LOW));

    // GPIO17 保持电池供电，唤醒引脚的上拉由 EXT1 休眠流程保持。
    ESP_ERROR_CHECK(rtc_gpio_hold_en(ZECTRIX_VBAT_LATCH));

    ESP_LOGI(kTag, "deep sleep until %04d-%02d-%02d 00:00:00, RTC ALARM GPIO5, BOOT GPIO0, latch held",
             next.tm_year + 1900, next.tm_mon + 1, next.tm_mday);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

}  // namespace

extern "C" void app_main(void) {
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const uint64_t wake_pins = esp_sleep_get_ext1_wakeup_status();
    bool user_window_pending = cause == ESP_SLEEP_WAKEUP_EXT1 &&
                               (wake_pins & (1ULL << ZECTRIX_BUTTON_OK)) != 0;
    ESP_LOGI(kTag, "wake cause=%d ext1=0x%llx reset_reason=%d", static_cast<int>(cause),
             static_cast<unsigned long long>(wake_pins), static_cast<int>(esp_reset_reason()));
    ESP_ERROR_CHECK(setenv("TZ", "CST-8", 1) == 0 ? ESP_OK : ESP_FAIL);
    tzset();
    InitNvs();
    for (gpio_num_t pin : kWakePins) {
        ESP_ERROR_CHECK(rtc_gpio_hold_dis(pin));
        ESP_ERROR_CHECK(rtc_gpio_deinit(pin));
    }

    static ZectrixBoard board;
    ESP_ERROR_CHECK(board.Init());
    ESP_ERROR_CHECK(xTaskCreate(ResetButtonTask, "calendar_reset", 3072, nullptr, 5,
                               nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    RtcPcf8563* rtc = board.rtc();
    if (rtc == nullptr) {
        ESP_LOGE(kTag, "RTC 不可用，无法设置每日唤醒");
        ESP_ERROR_CHECK(ESP_ERR_NOT_FOUND);
    }
    SeedSystemClockFromRtc(rtc);
#ifdef CALENDAR_RTC_CONSOLE
    CalendarRtcConsole(rtc);
#endif
    ESP_ERROR_CHECK(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    ESP_ERROR_CHECK(rtc->StopCountdownTimer() ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(rtc->ClearTimerFlag() ? ESP_OK : ESP_FAIL);

    zectrix_epd_config_t config = {};
    zectrix_epd_get_default_config(&config);
    zectrix_epd_handle_t epd = nullptr;
    ESP_ERROR_CHECK(zectrix_epd_new(&config, &epd));

    EnsureProvisioned(epd);

    RunAlmanacSelfCheck();
    RunFontSelfCheck();
    LogTodayAlmanac(&board);
    RunRenderSelfCheck();
    RunSchedSelfCheck();

    for (;;) {
        ESP_ERROR_CHECK(rtc->DisableAlarm() ? ESP_OK : ESP_FAIL);
        ESP_ERROR_CHECK(rtc->ClearAlarmFlag() ? ESP_OK : ESP_FAIL);
        const tm displayed = RenderTodayPage(ReadCalendarTime(rtc), &board);
        ESP_ERROR_CHECK(zectrix_epd_power_on(epd));
        ESP_ERROR_CHECK(zectrix_epd_refresh_full_1bpp(epd, g_canvas.data(),
                                                    g_canvas.size()));
        ESP_ERROR_CHECK(zectrix_epd_power_off(epd));

        if (user_window_pending) {
            WaitForUser();
            user_window_pending = false;
        }

        const tm now = ReadCalendarTime(rtc);
        if (!CalendarSameDate(displayed, now)) {
            continue;
        }
        tm next = {};
        ESP_ERROR_CHECK(CalendarNextMidnight(now, &next) ? ESP_OK : ESP_ERR_INVALID_STATE);
        ESP_ERROR_CHECK(rtc->SetAlarm(next) ? ESP_OK : ESP_FAIL);

        // 刷屏或设置闹钟期间跨过零点时，立即刷新新日期。
        if (!CalendarSameDate(displayed, ReadCalendarTime(rtc))) {
            continue;
        }
        ESP_ERROR_CHECK(zectrix_epd_del(epd));
        SleepUntilNextWake(&board, next);
    }
}
