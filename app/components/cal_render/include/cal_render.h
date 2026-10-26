/* 台历页渲染器 —— `tools/calendar_A_final.py` 的固件侧对等实现。
 *
 * 版式真源是 PC 侧的 `tools/calendar_A_final.py` + `tools/calendar_preview.py`，
 * 规格文档 `docs/03` §1。**改版式先改 Python，再同步本文件与文档**。
 *
 * 本组件**零 ESP-IDF 依赖**（只用 cal_font + cal_almanac + stdint/stddef）——
 * 故意的：同一份源码会在 PC 上用宿主编译器再编一遍，与 Python 渲染器做**逐像素**
 * 比对（`tools/cal_render_test.py`）。渲染器是全页唯一「坐标错 1px 就整屏歪」的地方，
 * 靠肉眼看图不足以验收。
 *
 * 画布是 400×300、1bpp、行优先、MSB-first、**1 = 白 / 0 = 黑**，与
 * `zectrix_epd_refresh_full_1bpp()` 要求的格式**完全一致** → 渲染结果直接送屏，
 * 中间不需要任何转换。
 *
 * 为什么不直接复用 `zectrix_demo_ui::ZectrixCanvas`：它挂在 `zectrix_epd.h` 上
 * （拉进 driver/gpio.h 等 IDF 头），一用就没法在 PC 上编。位图布局刻意与它逐位
 * 相同，设备侧把 `ZectrixCanvas::data()` 传进来即可 —— 不存在两份绘制实现。
 */
#ifndef CAL_RENDER_H
#define CAL_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cal_almanac.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 帧缓冲 */

#define CAL_FRAME_WIDTH 400
#define CAL_FRAME_HEIGHT 300
#define CAL_FRAME_STRIDE (CAL_FRAME_WIDTH / 8)                 /* 50 */
#define CAL_FRAME_BYTES (CAL_FRAME_STRIDE * CAL_FRAME_HEIGHT)  /* 15000 */

typedef struct {
    uint8_t* data; /* 必须至少有 CAL_FRAME_BYTES 字节 */
    int width;
    int height;
} CalFrame;

void cal_frame_clear(CalFrame* f, bool white);
void cal_frame_pixel(CalFrame* f, int x, int y, bool black);
void cal_frame_fill_rect(CalFrame* f, int x, int y, int w, int h, bool black);
/* 与 Python `Canvas.hline/vline` 同语义：坐标为**左上角 + 尺寸**，不做 inclusive 减一。 */
void cal_frame_hline(CalFrame* f, int y, int x0, int x1, int thickness);
void cal_frame_vline(CalFrame* f, int x, int y0, int y1, int thickness);

/* ---------------------------------------------------------------- 页面 */

/* 一页的全部输入。
 *
 * 数据契约（docs/03 §2）：第三方**只**给下面 4 项；星期 / 农历 / 节气 / 节日
 * 全部由 `cal_day_info_get()` 离线算出来。
 *
 * ⚠️ am / pm 的语义是「**第 1 段 / 第 2 段**」而不是真的上午下午 —— 夜班给的是
 * `20:00-24:00` / `00:00-04:00`。所以前缀**必须按开始时间判**（见
 * cal_span_with_prefix），写死「上午」就会出现「上午20:00-24:00」。
 */
typedef enum {
    CAL_BATTERY_NORMAL = 0,
    CAL_BATTERY_CHARGING,
    CAL_BATTERY_FULL,
    CAL_BATTERY_ABSENT,
    CAL_BATTERY_FAULT,
} CalBatteryState;

typedef struct {
    int percent; /* 原始估算百分比，范围 0～100 */
    CalBatteryState state;
} CalBatteryInput;

/* 电量左侧的信号格。connected 为 false 时画交叉斜线表示断开。
 * rssi 只在 connected 为 true 时参与分档：≥-55 四根、≥-65 三根、≥-75 两根、更弱一根。 */
typedef struct {
    bool connected;
    int rssi;
} CalWifiInput;

typedef struct {
    const cal_day_info* info; /* 由 cal_day_info_get() 填好 */
    const char* shift;        /* 原始班次名，可含全角括号；"" / NULL = 空 */
    const char* am;           /* 原始第 1 段，如 "08:00-12:00"；不带到站前缀 */
    const char* pm;           /* 原始第 2 段 */
    bool blank_schedule;     /* true 时班次和时段保持空白 */
    const CalBatteryInput* battery; /* NULL 时隐藏电量 */
    const CalWifiInput* wifi;       /* NULL 时隐藏信号格 */
} CalPageInput;

/* 渲染整页。info 为 NULL 时整帧只清成白色。 */
void cal_render_page(CalFrame* f, const CalPageInput* in);

/* ---------------------------------------------------------------- 纯函数（可单测） */

/* 班次名归一化：全角 → 半角、去首尾空白。写进 out（含 NUL），返回字节数。
 *
 * 第三方格式不统一：实测同一班组写成 `9DP（绿）`（全角）或 `9DP(绿)`（半角），
 * 全角括号把宽度从 107px 顶到 141px → 被迫缩字号。归一化一步白省 34px。
 */
int cal_shift_normalize(const char* src, char* out, size_t out_size);

/* 休息日判定：归一化后含「休」字（实测形态是单个 `休`，放宽为包含以兼容「休息」）。 */
bool cal_shift_is_rest(const char* shift);

/* 时段补前缀：`08:00-12:00` → `上午08:00-12:00`。
 * 空值原样返回；已带前缀不重复加；解析不出开始小时则原样返回。返回字节数。 */
int cal_span_with_prefix(const char* span, char* out, size_t out_size);

/* 从 `08:00-12:00` / `8:00-12:00` / `8：00-12：00` 取开始小时；取不到返回 -1。 */
int cal_span_start_hour(const char* span);

/* 反白班次块的中心 x：优先锚定基准 320，过宽时平移，保证既不撞左标签也不出屏。 */
int cal_shift_box_cx(int box_w);

/* 指定 pt 下整串的墨迹宽（只量不画）；字号组不存在或整串画不出返回 0。 */
int cal_text_width_pt(int pt, const char* utf8);

/* ---------------------------------------------------------------- 版式常量（自检用） */

/* 版式常量的快照。真源是 `tools/calendar_preview.py` / `calendar_A_final.py`
 * 顶部的同名常量 —— 板端自检拿它跟自己算出的几何对账，避免两边悄悄漂移。 */
typedef struct {
    int width, height, div_x;
    int week_cx, week_pt, week_top, week_step, rest_pt;
    int right_x, fest_pt, fest_pad_x, fest_gap, fest_box_h, fest_cy, fest_top_gap;
    int term_cx, term_pt, term_step, term_cy;
    int shift_cx, shift_cy, shift_pt, shift_pt_min, shift_pad_x, shift_box_h;
    int shift_box_max, shift_left_min, shift_right_max;
} CalLayout;

const CalLayout* cal_layout(void);

#ifdef __cplusplus
}
#endif

#endif /* CAL_RENDER_H */
