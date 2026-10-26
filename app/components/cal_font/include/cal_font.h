#ifndef CAL_FONT_H
#define CAL_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 台历页点阵字库。
//
// 字模由 tools/font_bitmap.py 用 PC 侧的 Noto Sans SC（wght=700）生成；**字号清单**
// 来自 tools/pt_census.py 对 1900–2099 全部真实内容的普查（25 个字号组）——
// 渲染器按墨迹高二分选字号，同一区域会落在一批不同 pt 上，手写清单必漏。
// 每个汉字按实际用到的字号各存一份 —— 台历页所有文字都是固定字号绘制，
// 不存在运行时缩放。
//
// 固件端选字号**不能复刻那个二分**（搜索区间 6..400，路径上会碰 41/54/104/203
// 这类大 pt，单 pt203 的中文点阵就要 5 KB/字）。正确做法：在本字库已有的 pt
// 候选集内**降序遍历、取第一个墨迹高 ≤ target_h 的** —— 与二分的「最大满足 pt」
// 结果等价，已由 `tools/pt_census.py --verify` 用 7469 例实测证明。
//
// 每个字形存两个盒子，缺一不可：
//   * 度量盒 bbox_*：来自 Pillow 的 textbbox()，用来算整串墨迹包围盒、把文本
//     对齐到目标锚点；
//   * 点阵盒 ink_*：实际栅格化后的墨迹范围，用来贴图。
// 两者常常差 1 像素（FreeType 的字形度量范围不等于位图范围），只存一个盒子
// 就会与 PC 侧渲染器的输出错位。tools/font_verify.py 用这个模型与 Pillow 的
// 实际绘制做逐像素比对。
//
// ⚠️ 绘制路径**永不终止进程**（2026-10-14 定的 B 方案，见 CAL_MISSING_ADVANCE_PX）。
// 班次名来自第三方数据源、词表不可穷举 —— 数据源冒出一个新词就让设备 abort，
// 结果是当天不再刷新（一天只醒一次），比画一张缺字的图糟得多。

typedef struct {
    uint16_t codepoint;  // Unicode 码点（全部在 BMP 内）
    int8_t bbox_dx;      // 度量盒左边界相对笔位
    int8_t bbox_dy;      // 度量盒上边界相对文本原点（升部线）
    uint8_t bbox_w;      // 度量盒宽
    uint8_t bbox_h;      // 度量盒高
    int8_t ink_dx;       // 点阵盒左边界相对文本原点
    int8_t ink_dy;       // 点阵盒上边界相对文本原点
    uint8_t ink_w;       // 点阵盒宽
    uint8_t ink_h;       // 点阵盒高
    uint8_t advance;     // 笔位步进（像素）
    uint16_t offset;     // 在所属字号组点阵池中的字节偏移
} CalGlyph;

typedef struct {
    uint8_t pt;              // 字号
    uint16_t count;          // 字形个数
    const CalGlyph* glyphs;  // 按码点升序
    const uint8_t* bitmap;   // 行对齐 MSB-first、1 表示墨迹
} CalFontGroup;

extern const CalFontGroup kCalFontGroups[];
extern const size_t kCalFontGroupCount;

typedef enum {
    CAL_ALIGN_CENTER = 0,
    CAL_ALIGN_LEFT = 1,
    CAL_ALIGN_RIGHT = 2,
} CalAlign;

// 缺字（某码点在本字号组里查不到字形）时的占位步进：不画、笔位前进半字宽。
//
// 全角汉字在 pt 下的步进约等于 pt，故取 `pt / 2` —— 画面上留一个约半字的空档，
// 其余字符位置照常排下去。这样「写错一个字」的代价只是两个字符间多一个缝，
// 而不是整屏空白或设备当机。
//
// 这是 2026-10-14 决定的 A/B/C 三选一里的 **B**（见 docs/03 §5 #24）。
// 字库漂移改由三道**启动期**自检兜底，不靠运行期终止：
//   ① tools/font_verify.py 的字符级断言（PC，所有用例）；
//   ② tools/pt_census.py 的形态表 ↔ 字符集双向检查（PC，普查前）；
//   ③ app_main.cc::RunFontSelfCheck 的班次名词汇守卫（板端，每次开机）。
#define CAL_MISSING_ADVANCE_PX(pt) ((pt) / 2)

typedef struct {
    int width;     // 整串度量盒宽
    int height;    // 整串度量盒高
    int origin_x;  // 文本原点 x（升部线左端）
    int origin_y;  // 文本原点 y（升部线）
    int missing;   // 串中被跳过的字符数（缺字或非法 UTF-8 字节），0 = 全部画得出
} CalTextMetrics;

// 取指定字号的字形表；字号不存在返回 NULL。
const CalFontGroup* cal_font_group(int pt);

// 在指定字号里按码点查字形；查不到返回 NULL。
const CalGlyph* cal_font_lookup(int pt, uint32_t codepoint);

// 取字形点阵第 row 行第 column 列是否为墨迹。
bool cal_glyph_pixel(const CalFontGroup* group, const CalGlyph* glyph, int row,
                     int column);

// 按 (cx, cy) 锚点与对齐方式算出整串度量与文本原点。
// 空串、整串一个字形都画不出、字号组不存在时返回 false。
// 串中缺字**就地跳过**（笔位前进 CAL_MISSING_ADVANCE_PX(pt)，不计入包围盒），
// 跳过个数回填 out->missing —— 本函数不终止进程。
bool cal_text_layout(int pt, const char* utf8, CalAlign align, int cx, int cy,
                     CalTextMetrics* out);

// 选字号：挑出字库里**能完整画出 utf8、且整串度量盒高 ≤ target_h 的最大 pt**。
//
// 与 PC 侧 `canvas_preview.Canvas.font_for_height()` 等价（同一个判据：
// 整串 textbbox 高 ≤ target_h，取满足条件的最大 pt）。台历页里「按墨迹高选字号」
// 的区域都用它（顶栏农历、主区时段、底栏标签），固定 pt 的区域不用。
//
// ⚠️ 两条判据都**不可少**（2026-10-14 板端自检抓出的真 bug，两个原因叠加）：
//   ① 必须取**最大**满足 pt —— 不能依赖 kCalFontGroups 的排列顺序「取第一个满足的」。
//      字库数据是按 pt 升序生成的（`font_bitmap.py` 用的是 `reverse=True` 但下游
//      又排回来过），取第一个满足的会退化成**最小** pt，整页字号全线塌到 pt14。
//   ② 必须要求 `missing == 0` —— 缺字不进包围盒（B 方案），于是「能画出来的那几个字」
//      单独算出来的高度会**虚小**，让一个根本画不全的字号组轻易满足 `height <= target_h`。
//
// 返回 -1 = 没有任何字号组能完整画出该串；调用方应降级（不画 / 换占位文案）。
// utf8 为 NULL 或空串时返回 -1。**不终止进程，也不打日志**（选字号会被频繁调用，
// 让缺字告警只在真正绘制时打一次）。
int cal_font_pick_pt(const char* utf8, int target_h);

// 逐字遍历：cursor.pen 是当前字形的笔位，cursor.glyph 是当前字形。
//
// 缺字**不会**出现在遍历里 —— cal_text_cursor_next() 会静默越过它们并让 pen
// 跳过对应的半字宽缺口，所以绘制循环可以无条件地「有 glyph 就在 pen 处贴图」。
// 跳过的字符数记在 cursor.skipped，首个缺失码点在 cursor.first_skipped（便于日志）。
typedef struct {
    const CalFontGroup* group;  // NULL = 字号组不存在（此时 next() 恒返回 false）
    const char* cursor;
    const CalGlyph* glyph;
    int pen;       // 当前字形的笔位（相对文本原点）
    int next_pen;  // 下一个字形的笔位
    int skipped;            // 已跳过的字符数
    uint32_t first_skipped; // 首个被跳过的码点，0 = 无
} CalTextCursor;

void cal_text_cursor_init(int pt, const char* utf8, CalTextCursor* cursor);
bool cal_text_cursor_next(CalTextCursor* cursor);

// 数出 utf8 里有多少个字符在 pt 字号组中**查不到字形**，并回填首个缺失码点
// （out_first_missing 可为 NULL）。非法 UTF-8 字节也算一个缺失字符。**不终止进程。**
//
// 用途：绘制前先探一次，好把「画面上有个空档」的原因写进日志。台历页正文都是
// 离线算出的固定内容，唯一的内容域不受控处是班次名（第三方数据源）。绘制本身
// 已按 B 方案跳过缺字，本函数只用于**观测与告警**，不是绘制的前置条件。
// pt 字号组不存在时返回 -1。
int cal_font_missing_count(int pt, const char* utf8, uint32_t* out_first_missing);

#ifdef __cplusplus
}
#endif

#endif  // CAL_FONT_H
