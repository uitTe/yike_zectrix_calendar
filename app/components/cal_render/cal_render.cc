#include "cal_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cal_font.h"

// 台历页渲染器 —— 严格照搬 `tools/calendar_A_final.py` + `tools/calendar_preview.py`
// 的常量与执行顺序。**改版式先改 Python，再同步这里**（docs/03 §1）。
//
// 文件分四段：版式常量 → 帧缓冲原语 → 文字绘制内核 → 纯函数与整页。
// 之所以把 Python 的每一步都留着同名的中间量（left_limit / avail / chosen …），
// 是为了以后改版式时能一行一行对着看，而不是隔着两层抽象猜。

namespace {

// ---------------------------------------------------------------- 版式常量
//
// 全部来自 calendar_preview.py 顶部与 calendar_A_final.py 顶部。**不要就地改值** ——
// 改了就跟 PC 侧渲染器分叉，宿主逐像素比对（tools/cal_render_test.py）会立刻红。

constexpr int kWidth = CAL_FRAME_WIDTH;
constexpr int kHeight = CAL_FRAME_HEIGHT;

constexpr int kDivX = 232;      // 主区左右分栏竖线 x
constexpr int kTopLineY = 52;   // 顶栏分隔线 y
constexpr int kBotLineY = 232;  // 底栏分隔线 y（= 主区下沿）
constexpr int kMainTop = 62;    // 主区上沿（竖线起点）

constexpr int kWeekCx = 316;
constexpr int kWeekPt = 44;
constexpr int kWeekTop = 104;
constexpr int kWeekStep = 52;
constexpr int kRestPt = 72;

constexpr int kRightX = kWidth - 16;  // 顶栏内容右对齐基准 = 384
constexpr int kFestPt = 18;
constexpr int kFestPtMin = 14;
constexpr int kFestPadX = 10;
constexpr int kFestBoxH = 26;
constexpr int kFestGap = 14;
constexpr int kFestTopGap = 16;
constexpr int kFestCy = 38;
constexpr char kFestJoin[] = "·";

// (字号, 内边距, 间隙) —— 总宽超预算时逐档下探，**先缩内边距、后缩字号**：
// 反白签的黑框本身占视觉重量，缩 pad 比缩字号更不伤观感，也不会让同一天的两个签
// 大小不一。首档 = 定稿值，单签永远命中它。
struct FestScale {
    int pt, pad, gap;
};
constexpr FestScale kFestScales[] = {
    {18, 10, 14}, {18, 9, 13}, {18, 8, 12}, {17, 8, 12},
    {16, 7, 11},  {15, 6, 10}, {14, 6, 9},
};

constexpr int kTermCx = 367;
constexpr int kTermPt = 31;
constexpr int kTermStep = 46;
constexpr int kTermCy = 156;  // = kWeekTop + kWeekStep，两列垂直对齐

constexpr int kShiftCx = 320;
constexpr int kShiftCy = 266;
constexpr int kShiftPt = 28;
constexpr int kShiftPtMin = 20;
constexpr int kShiftPadX = 18;
constexpr int kShiftBoxH = 52;
constexpr int kShiftBoxMax = 200;    // 块宽**软**上限：超过则收缩字号
constexpr int kShiftLeftMin = 120;   // 块左边界**硬**下限（左标签墨迹右界 106 + 14px）
constexpr int kShiftRightMax = 392;  // 块右边界**硬**上限（右边距 5px）

constexpr char kFailText[] = "获取失败";
constexpr char kRestText[] = "休息";
constexpr int kBatteryPt = 18;
constexpr int kBatteryCy = 70;

// 主区左栏几何（variant_a 内部量）
constexpr int kMainLeftX0 = 16;
constexpr int kMainLeftX1 = 220;
constexpr int kMainMidY = (kMainTop + kBotLineY) / 2;  // 147
constexpr int kSpanGap = 60;
constexpr int kMaxTextH = 30;  // 时段 / 「获取失败」的起始字号
constexpr int kMinTextH = 18;  // 收缩下限

bool StartsWith(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool IsAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// 就地去掉首尾 ASCII 空白（= Python `str.strip()` 的常用语义）。
void StripAscii(char* s) {
    size_t n = strlen(s);
    size_t begin = 0;
    while (begin < n && IsAsciiSpace(s[begin])) {
        ++begin;
    }
    size_t end = n;
    while (end > begin && IsAsciiSpace(s[end - 1])) {
        --end;
    }
    const size_t len = end - begin;
    if (begin > 0) {
        memmove(s, s + begin, len);
    }
    s[len] = '\0';
}

// 取一个 UTF-8 字符（最多 4 字节）到 out（含 NUL）并前移 cursor。返回字节数。
int NextChar(const char** cursor, char* out) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(*cursor);
    int len = 1;
    if (s[0] >= 0xF0) {
        len = 4;
    } else if (s[0] >= 0xE0) {
        len = 3;
    } else if (s[0] >= 0xC0) {
        len = 2;
    }
    int i = 0;
    for (; i < len; ++i) {
        if (s[i] == 0) {
            break;
        }
        out[i] = static_cast<char>(s[i]);
    }
    out[i] = '\0';
    *cursor += i;
    return i;
}

// ---------------------------------------------------------------- 文字内核

// 贴一个字形。x/y 是**点阵盒左上角**在屏幕上的位置。
void BlitGlyph(CalFrame* f, const CalFontGroup* group, const CalGlyph* glyph, int x,
               int y, bool black) {
    for (int row = 0; row < glyph->ink_h; ++row) {
        for (int col = 0; col < glyph->ink_w; ++col) {
            if (cal_glyph_pixel(group, glyph, row, col)) {
                cal_frame_pixel(f, x + col, y + row, black);
            }
        }
    }
}

// 按 pt 画一整串（= Python `Canvas.text_pt()`）。
//
// 定位：度量盒 bbox 决定对齐，点阵盒 ink 决定贴图 —— 两个盒子缺一不可，
// 只用一个都会与 Pillow 差 1px（见 cal_font.h 顶部说明）。
bool DrawTextPt(CalFrame* f, int pt, const char* utf8, CalAlign align, int cx, int cy,
                bool black) {
    CalTextMetrics m = {};
    if (!cal_text_layout(pt, utf8, align, cx, cy, &m)) {
        return false;
    }
    CalTextCursor cur;
    cal_text_cursor_init(pt, utf8, &cur);
    while (cal_text_cursor_next(&cur)) {
        const CalGlyph* glyph = cur.glyph;
        BlitGlyph(f, cur.group, glyph, m.origin_x + cur.pen + glyph->ink_dx,
                  m.origin_y + glyph->ink_dy, black);
    }
    return true;
}

// 按「目标墨迹高」画一整串（= Python `Canvas.text()`）：先在字库里挑字号，再按 pt 画。
bool DrawTextH(CalFrame* f, const char* utf8, int target_h, CalAlign align, int cx,
               int cy, bool black) {
    const int pt = cal_font_pick_pt(utf8, target_h);
    if (pt < 0) {
        return false;
    }
    return DrawTextPt(f, pt, utf8, align, cx, cy, black);
}

// = Python `Canvas.text_width()`。
int TextWidthH(const char* utf8, int target_h) {
    const int pt = cal_font_pick_pt(utf8, target_h);
    if (pt < 0) {
        return 0;
    }
    return cal_text_width_pt(pt, utf8);
}

// 反白块（黑底白字，固定块高、宽度自适应） = Python `Canvas.reversed_box_pt()`。
//
// 固定块高是有理由的：不同班次名的墨迹高差很多（`9DP1` 全拉丁只有 21px、
// 纯中文 28px），块高若跟着走，底栏就会一天胖一天瘦。
void DrawReversedBoxPt(CalFrame* f, int pt, const char* utf8, int cx, int cy,
                       int box_h, int pad_x) {
    CalTextMetrics probe = {};
    if (!cal_text_layout(pt, utf8, CAL_ALIGN_LEFT, 0, 0, &probe)) {
        return;
    }
    const int box_w = probe.width + pad_x * 2;
    const int x0 = cx - box_w / 2;
    const int y0 = cy - box_h / 2;
    cal_frame_fill_rect(f, x0, y0, box_w, box_h, true);
    // 白字：锚在 (x0 + pad_x) 这个 bbox 左界上，与 Python 的
    // `text((x0 + pad_x - b[0], cy - th//2 - b[1]))` 等价。
    DrawTextPt(f, pt, utf8, CAL_ALIGN_LEFT, x0 + pad_x, cy, false);
}

// ---------------------------------------------------------------- 顶栏右侧

// 一排反白签的总宽（含间隙） = Python `_fest_row_width()`。
int FestRowWidth(int count, const char* const* fests, int pt, int pad, int gap) {
    int total = 0;
    for (int i = 0; i < count; ++i) {
        total += cal_text_width_pt(pt, fests[i]) + 2 * pad;
    }
    if (count > 1) {
        total += gap * (count - 1);
    }
    return total;
}

// 挑第一个能塞进 avail 的档位；全都不行返回 nullptr = Python `_pick_scale()`。
const FestScale* PickFestScale(int count, const char* const* fests, int avail) {
    for (const FestScale& s : kFestScales) {
        if (FestRowWidth(count, fests, s.pt, s.pad, s.gap) <= avail) {
            return &s;
        }
    }
    return nullptr;
}

// 兜底：全部档位都塞不下 → 并成**一个**签（`中秋节·国庆节`）= Python `_draw_merged()`。
// 实测 1900–2099 走不到这里（最坏的两个 3 字节日在第二档就放下了），留着是为了
// 「以后往节日表里加长名字」时不至于把内容画出屏。
void DrawMergedFests(CalFrame* f, int count, const char* const* fests, int avail) {
    constexpr size_t kScaleCount = sizeof(kFestScales) / sizeof(kFestScales[0]);
    int pt = kFestScales[kScaleCount - 1].pt;
    int pad = kFestScales[kScaleCount - 1].pad;

    char merged[96] = "";
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            strncat(merged, kFestJoin, sizeof(merged) - strlen(merged) - 1);
        }
        strncat(merged, fests[i], sizeof(merged) - strlen(merged) - 1);
    }

    int total = cal_text_width_pt(pt, merged) + 2 * pad;
    while (pt > kFestPtMin && total > avail) {
        --pt;
        total = cal_text_width_pt(pt, merged) + 2 * pad;
    }
    while (pad > 4 && total > avail) {
        --pad;
        total = cal_text_width_pt(pt, merged) + 2 * pad;
    }
    DrawReversedBoxPt(f, pt, merged, kRightX - total / 2, kFestCy, kFestBoxH, pad);
}

// 顶栏右侧：农历一行；有节日时加第二行反白签（同日多个就并排）
// = Python `draw_top_right()`。
void DrawTopRight(CalFrame* f, const char* lunar, int count,
                  const char* const* fests, int left_limit) {
    const int avail = kRightX - left_limit;

    if (count == 0) {
        // 无节日 → 农历单行居中于标题。
        int h = 21;
        while (h > 18 && TextWidthH(lunar, h) > avail) {
            --h;
        }
        DrawTextH(f, lunar, h, CAL_ALIGN_RIGHT, kRightX, 30, true);
        return;
    }

    // 有节日 → 农历上移一行，签行右对齐于 kRightX。
    int h = 18;
    while (h > 15 && TextWidthH(lunar, h) > avail) {
        --h;
    }
    DrawTextH(f, lunar, h, CAL_ALIGN_RIGHT, kRightX, 17, true);

    const FestScale* chosen = PickFestScale(count, fests, avail);
    if (chosen == nullptr) {
        DrawMergedFests(f, count, fests, avail);
        return;
    }

    int x = kRightX - FestRowWidth(count, fests, chosen->pt, chosen->pad, chosen->gap);
    for (int i = 0; i < count; ++i) {
        const int bw = cal_text_width_pt(chosen->pt, fests[i]) + 2 * chosen->pad;
        DrawReversedBoxPt(f, chosen->pt, fests[i], x + bw / 2, kFestCy, kFestBoxH,
                          chosen->pad);
        x += bw + chosen->gap;
    }
}

// 节气竖排于星期右侧。**有则显示，没有就整列不画**（星期列位置不受影响）。
void DrawTermVertical(CalFrame* f, const char* term) {
    if (term == nullptr || term[0] == '\0') {
        return;
    }
    char chars[4][8];
    int n = 0;
    const char* cursor = term;
    while (*cursor != '\0' && n < 4) {
        if (NextChar(&cursor, chars[n]) == 0) {
            break;
        }
        ++n;
    }
    for (int i = 0; i < n; ++i) {
        // 与 Python `TERM_CY + int((i - (n - 1) / 2) * TERM_STEP)` 逐位一致：
        // 先算偏移再取整，避免 (double) 与 int 的收尾差异。
        const int offset = static_cast<int>((i - (n - 1) / 2.0) * kTermStep);
        DrawTextPt(f, kTermPt, chars[i], CAL_ALIGN_CENTER, kTermCx, kTermCy + offset,
                   true);
    }
}

// ---------------------------------------------------------------- 信号格
//
// 与 tools/calendar_A_final.py 的 draw_wifi 逐像素对等。4px 宽的竖条画不出 2px 边框的
// 空心（cal_frame box 的 outline 宽度就是 2），所以「断开」不用空心，改用占满整个图标区
// 的 ×，两种状态的形状差异最大。

// 电池左侧 7px 是低电量感叹号的位置，信号格再往左留 3px 空隙。
constexpr int kWifiGap = 10;
constexpr int kWifiBarW = 4;
constexpr int kWifiBarGap = 2;
constexpr int kWifiBarH[] = {4, 7, 10, 13};
constexpr int kWifiTotalW = kWifiBarW * 4 + kWifiBarGap * 3;  // 22

int WifiLevel(int rssi) {
    // 已连接至少亮 1 根，不出现「连上了却没有格子」。
    if (rssi >= -55) {
        return 4;
    }
    if (rssi >= -65) {
        return 3;
    }
    if (rssi >= -75) {
        return 2;
    }
    return 1;
}

void DrawWifi(CalFrame* f, int x_right, int y_bottom, const CalWifiInput& wifi) {
    const int x0 = x_right - kWifiTotalW;
    if (wifi.connected) {
        const int level = WifiLevel(wifi.rssi);
        for (int i = 0; i < 4; ++i) {
            if (i >= level) {
                continue;
            }
            const int bx = x0 + i * (kWifiBarW + kWifiBarGap);
            cal_frame_fill_rect(f, bx, y_bottom - kWifiBarH[i], kWifiBarW,
                                kWifiBarH[i], true);
        }
        return;
    }
    constexpr int span = kWifiTotalW - 1;
    constexpr int height = kWifiBarH[3] - 1;
    for (int i = 0; i < kWifiTotalW; ++i) {
        const int down = y_bottom - 1 - i * height / span;
        const int up = y_bottom - kWifiBarH[3] + i * height / span;
        cal_frame_fill_rect(f, x0 + i, down, 1, 1, true);
        cal_frame_fill_rect(f, x0 + i, up, 1, 1, true);
    }
}

void DrawBattery(CalFrame* f, const CalBatteryInput& battery,
                 const CalWifiInput* wifi) {
    if (battery.percent < 0 || battery.percent > 100 ||
        battery.state < CAL_BATTERY_NORMAL || battery.state > CAL_BATTERY_FAULT) {
        abort();
    }
    const bool unavailable = battery.state == CAL_BATTERY_ABSENT ||
                             battery.state == CAL_BATTERY_FAULT;
    const int shown = battery.state == CAL_BATTERY_FULL ? 100 : battery.percent / 5 * 5;
    char label[8];
    if (unavailable) {
        snprintf(label, sizeof(label), "--%%");
    } else {
        snprintf(label, sizeof(label), "%d%%", shown);
    }
    const int x = kRightX - cal_text_width_pt(kBatteryPt, label) - 28;
    const int y = kBatteryCy - 6;
    if (wifi != nullptr) {
        DrawWifi(f, x - kWifiGap, y + 12, *wifi);
    }
    cal_frame_fill_rect(f, x, y, 20, 12, true);
    cal_frame_fill_rect(f, x + 2, y + 2, 16, 8, false);
    cal_frame_fill_rect(f, x + 20, y + 4, 2, 4, true);
    if (!unavailable) {
        cal_frame_fill_rect(f, x + 3, y + 3, 14 * shown / 100, 6, true);
    }
    if (battery.state == CAL_BATTERY_CHARGING) {
        constexpr int bolt[][2] = {{10, 2}, {9, 2}, {8, 2}, {7, 2},
                                    {6, 6}, {9, 2}, {8, 2}, {7, 2}};
        for (int row = 0; row < 8; ++row) {
            cal_frame_fill_rect(f, x + bolt[row][0] - 1, y + 1 + row,
                                bolt[row][1] + 2, 3, false);
        }
        for (int row = 0; row < 8; ++row) {
            cal_frame_fill_rect(f, x + bolt[row][0], y + 2 + row,
                                bolt[row][1], 1, true);
        }
    } else if (unavailable || (battery.state == CAL_BATTERY_NORMAL && battery.percent < 20)) {
        cal_frame_fill_rect(f, x - 7, y + 1, 2, 7, true);
        cal_frame_fill_rect(f, x - 7, y + 10, 2, 2, true);
    }
    CalTextMetrics metrics = {};
    if (!cal_text_layout(kBatteryPt, label, CAL_ALIGN_RIGHT, kRightX, kBatteryCy, &metrics) ||
        metrics.missing != 0 || !DrawTextPt(f, kBatteryPt, label, CAL_ALIGN_RIGHT,
                                          kRightX, kBatteryCy, true)) {
        abort();
    }
}

}  // namespace

// ---------------------------------------------------------------- 帧缓冲原语

extern "C" void cal_frame_clear(CalFrame* f, bool white) {
    if (f == nullptr || f->data == nullptr) {
        return;
    }
    memset(f->data, white ? 0xFF : 0x00, CAL_FRAME_BYTES);
}

extern "C" void cal_frame_pixel(CalFrame* f, int x, int y, bool black) {
    if (f == nullptr || f->data == nullptr || x < 0 || x >= kWidth || y < 0 ||
        y >= kHeight) {
        return;
    }
    uint8_t& byte = f->data[static_cast<size_t>(y) * CAL_FRAME_STRIDE + x / 8];
    const uint8_t mask = static_cast<uint8_t>(1U << (7 - (x & 7)));
    if (black) {
        byte &= static_cast<uint8_t>(~mask);  // 0 = 黑
    } else {
        byte |= mask;  // 1 = 白
    }
}

extern "C" void cal_frame_fill_rect(CalFrame* f, int x, int y, int w, int h,
                                    bool black) {
    for (int py = y; py < y + h; ++py) {
        for (int px = x; px < x + w; ++px) {
            cal_frame_pixel(f, px, py, black);
        }
    }
}

extern "C" void cal_frame_hline(CalFrame* f, int y, int x0, int x1, int thickness) {
    cal_frame_fill_rect(f, x0, y, x1 - x0, thickness, true);
}

extern "C" void cal_frame_vline(CalFrame* f, int x, int y0, int y1, int thickness) {
    cal_frame_fill_rect(f, x, y0, thickness, y1 - y0, true);
}

// ---------------------------------------------------------------- 纯函数

extern "C" int cal_span_start_hour(const char* span) {
    if (span == nullptr) {
        return -1;
    }
    const char* p = span;
    while (IsAsciiSpace(*p)) {
        ++p;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    int hour = 0;
    int digits = 0;
    while (digits < 2 && *p >= '0' && *p <= '9') {
        hour = hour * 10 + (*p - '0');
        ++p;
        ++digits;
    }
    while (IsAsciiSpace(*p)) {
        ++p;
    }
    // ':' 或 '：'（U+FF1A = EF BC 9A）
    const bool colon = (*p == ':') ||
                       (static_cast<unsigned char>(p[0]) == 0xEF &&
                        static_cast<unsigned char>(p[1]) == 0xBC &&
                        static_cast<unsigned char>(p[2]) == 0x9A);
    if (!colon) {
        return -1;
    }
    return (hour >= 0 && hour <= 23) ? hour : -1;
}

extern "C" int cal_span_with_prefix(const char* span, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (span == nullptr || span[0] == '\0') {
        return 0;
    }
    static const char* const kPrefixes[] = {"凌晨", "上午", "下午", "夜间"};
    for (const char* prefix : kPrefixes) {
        if (StartsWith(span, prefix)) {
            strncpy(out, span, out_size - 1);
            out[out_size - 1] = '\0';
            return static_cast<int>(strlen(out));
        }
    }
    // 前缀**按开始时间**判 —— 夜班给的是 `20:00-24:00`，写死「上午」就会出现
    // 「上午20:00-24:00」（实测确认过这种荒谬结果）。
    const int hour = cal_span_start_hour(span);
    if (hour < 0) {
        strncpy(out, span, out_size - 1);
        out[out_size - 1] = '\0';
        return static_cast<int>(strlen(out));
    }
    const char* prefix =
        hour < 6 ? "凌晨" : hour < 12 ? "上午" : hour < 18 ? "下午" : "夜间";
    snprintf(out, out_size, "%s%s", prefix, span);
    return static_cast<int>(strlen(out));
}

extern "C" int cal_shift_normalize(const char* src, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    size_t n = 0;
    if (src != nullptr) {
        // 与 Python 一致：先 strip，再逐字符转换。
        const char* begin = src;
        while (IsAsciiSpace(*begin)) {
            ++begin;
        }
        const char* end = begin + strlen(begin);
        while (end > begin && IsAsciiSpace(end[-1])) {
            --end;
        }
        const char* p = begin;
        while (p < end && n + 8 < out_size) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x80) {
                out[n++] = static_cast<char>(c);
                ++p;
                continue;
            }
            // 非 ASCII：解码一个码点，只处理下面两类改写，其余原样搬运。
            char buf[8];
            const int len = NextChar(&p, buf);
            if (len == 0) {
                break;
            }
            if (len == 3) {
                const uint32_t cp = (static_cast<uint32_t>(buf[0] & 0x0F) << 12) |
                                    (static_cast<uint32_t>(buf[1] & 0x3F) << 6) |
                                    static_cast<uint32_t>(buf[2] & 0x3F);
                if (cp == 0x3000) {  // 全角空格
                    out[n++] = ' ';
                    continue;
                }
                if (cp >= 0xFF01 && cp <= 0xFF5E) {  // 全角 ASCII（! 到 ~）
                    out[n++] = static_cast<char>(cp - 0xFEE0);
                    continue;
                }
            }
            for (int i = 0; i < len && n + 1 < out_size; ++i) {
                out[n++] = buf[i];
            }
        }
    }
    out[n] = '\0';
    return static_cast<int>(n);
}

extern "C" bool cal_shift_is_rest(const char* shift) {
    char normalized[64];
    cal_shift_normalize(shift, normalized, sizeof(normalized));
    // 实测形态是单个 `休`；放宽到「包含」是为了兼容 `休息` / `休假` / `休班`。
    return strstr(normalized, "休") != nullptr;
}

extern "C" int cal_shift_box_cx(int box_w) {
    int cx = kShiftCx;
    if (cx + (box_w + 1) / 2 > kShiftRightMax) {  // 右边界越界 → 左移
        cx = kShiftRightMax - (box_w + 1) / 2;
    }
    if (cx - box_w / 2 < kShiftLeftMin) {  // 左边界越界 → 右移（左标签优先）
        cx = kShiftLeftMin + box_w / 2;
    }
    return cx;
}

extern "C" int cal_text_width_pt(int pt, const char* utf8) {
    CalTextMetrics m = {};
    if (!cal_text_layout(pt, utf8, CAL_ALIGN_LEFT, 0, 0, &m)) {
        return 0;
    }
    return m.width;
}

extern "C" const CalLayout* cal_layout(void) {
    static const CalLayout layout = {
        kWidth,       kHeight,       kDivX,         kWeekCx,     kWeekPt,
        kWeekTop,   kWeekStep,     kRestPt,        kRightX,     kFestPt,
        kFestPadX,  kFestGap,      kFestBoxH,      kFestCy,     kFestTopGap,
        kTermCx,    kTermPt,       kTermStep,      kTermCy,     kShiftCx,
        kShiftCy,   kShiftPt,      kShiftPtMin,    kShiftPadX,  kShiftBoxH,
        kShiftBoxMax, kShiftLeftMin, kShiftRightMax,
    };
    return &layout;
}

// ---------------------------------------------------------------- 整页

extern "C" void cal_render_page(CalFrame* f, const CalPageInput* in) {
    if (f == nullptr || f->data == nullptr) {
        return;
    }
    cal_frame_clear(f, true);
    if (in == nullptr || in->info == nullptr) {
        return;
    }
    const cal_day_info& info = *in->info;

    const char* fests[CAL_FEST_MAX];
    const int fest_count = info.fest_count;
    for (int i = 0; i < fest_count; ++i) {
        fests[i] = info.festivals[i];
    }

    // ---- 顶栏：完整年月日 ----
    DrawTextH(f, info.date_cn, 27, CAL_ALIGN_LEFT, 16, 30, true);
    cal_frame_hline(f, kTopLineY, 0, kWidth, 2);

    // ---- 主区：分栏竖线 + 右侧竖排星期（逐字，固定 pt） ----
    cal_frame_vline(f, kDivX, kMainTop, kBotLineY, 2);
    {
        const char* cursor = info.weekday_cn;
        char one[8];
        int i = 0;
        while (*cursor != '\0' && i < 8) {
            if (NextChar(&cursor, one) == 0) {
                break;
            }
            DrawTextPt(f, kWeekPt, one, CAL_ALIGN_CENTER, kWeekCx,
                       kWeekTop + i * kWeekStep, true);
            ++i;
        }
    }

    // ---- 主区左：三态 —— 休息日 / 无数据（拉取失败）/ 正常时段 ----
    char am[64] = "";
    char pm[64] = "";
    cal_span_with_prefix(in->am, am, sizeof(am));
    cal_span_with_prefix(in->pm, pm, sizeof(pm));
    StripAscii(am);
    StripAscii(pm);

    if (!in->blank_schedule && cal_shift_is_rest(in->shift)) {
        DrawTextPt(f, kRestPt, kRestText, CAL_ALIGN_CENTER,
                   (kMainLeftX0 + kMainLeftX1) / 2, kMainMidY, true);
    } else if (!in->blank_schedule && am[0] == '\0' && pm[0] == '\0') {
        // 两行都空 = 没拿到数据（拉取失败且无可用缓存）。此时**不能**画「上午 / 下午」
        // 两个光秃秃的前缀 —— 那是空文本被拼了前缀留下来的壳。
        int h = kMaxTextH;
        while (h > kMinTextH && TextWidthH(kFailText, h) > (kMainLeftX1 - kMainLeftX0)) {
            --h;
        }
        DrawTextH(f, kFailText, h, CAL_ALIGN_CENTER,
                  (kMainLeftX0 + kMainLeftX1) / 2, kMainMidY, true);
    } else if (!in->blank_schedule) {
        const char* lines[2];
        int count = 0;
        if (am[0] != '\0') {
            lines[count++] = am;
        }
        if (pm[0] != '\0') {
            lines[count++] = pm;  // 只有一段数据就只画一行，不留空行
        }
        int h = kMaxTextH;
        while (h > kMinTextH) {
            int widest = 0;
            for (int i = 0; i < count; ++i) {
                const int w = TextWidthH(lines[i], h);
                if (w > widest) {
                    widest = w;
                }
            }
            if (widest <= (kMainLeftX1 - kMainLeftX0)) {
                break;
            }
            --h;
        }
        const int top = kMainMidY - (count - 1) * kSpanGap / 2;
        for (int i = 0; i < count; ++i) {
            DrawTextH(f, lines[i], h, CAL_ALIGN_LEFT, kMainLeftX0, top + i * kSpanGap,
                      true);
        }
    }

    // ---- 底栏：今日班次 + 反白班次名 ----
    cal_frame_hline(f, kBotLineY, 0, kWidth, 2);
    DrawTextH(f, "今日班次", 23, CAL_ALIGN_LEFT, 16, kShiftCy, true);

    char shift[64] = "";
    cal_shift_normalize(in->shift, shift, sizeof(shift));
    if (!in->blank_schedule && shift[0] != '\0') {
        int pt = kShiftPt;
        while (pt > kShiftPtMin &&
               cal_text_width_pt(pt, shift) + 2 * kShiftPadX > kShiftBoxMax) {
            --pt;
        }
        const int box_w = cal_text_width_pt(pt, shift) + 2 * kShiftPadX;
        DrawReversedBoxPt(f, pt, shift, cal_shift_box_cx(box_w), kShiftCy, kShiftBoxH,
                          kShiftPadX);
    }

    // ---- 顶栏右侧：农历 + 节日反白签；以及主区右的竖排节气 ----
    // 签行左界由**实测标题宽度**决定（标题会随 `2026年9月23日` / `2026年12月31日`
    // 长度微变，写死常量就会在某些月份压到标题）。
    // ⚠️ Python 这里量的是 **pt27 的宽**（而标题本身是按墨迹高 27 → pt28 画的），
    // 必须照抄：换成 pt28 会让 avail 变大、最坏情形下选到更大的档位，观感就变了。
    const int title_w = cal_text_width_pt(27, info.date_cn);
    DrawTopRight(f, info.lunar_cn, fest_count, fests, 16 + title_w + kFestTopGap);
    DrawTermVertical(f, info.term);
    if (in->battery != nullptr) {
        DrawBattery(f, *in->battery, in->wifi);
    }
}
