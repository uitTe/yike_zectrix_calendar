// cal_font 组件的宿主侧验证：**缺字跳过（B 方案）** + **选字号（cal_font_pick_pt）**。
//
// 2026-10-14 决定：绘制路径**永不终止进程**。班次名来自第三方数据源、词表不可
// 穷举，若遇到字库没有的字符就 abort，代价是当天不再刷新（设备一天只醒一次），
// 远大于画面上多一个空档。于是改为「缺字就地跳过、笔位前进半字宽」。
//
// 这个测试针对的正是「跳过之后位置对不对」—— 它无法用 Pillow 做参照（Pillow 有
// 那个字的字形，会照常画出来），所以判据是**结构性的**：
//     带缺字串的包围盒 == 去掉缺字后的包围盒 + 缺字数 × CAL_MISSING_ADVANCE_PX(pt)
// 以及逐字游标的字形序列与笔位（除缺字后的第一个字形整体右移外，其余完全相同）。
//
// 用宿主 g++ 直接编 app/components/cal_font/cal_font.cc（纯逻辑，只依赖 esp_log，
// 由 tools/stub/esp_log.h 替身接住）。跑法：python tools/font_missing_test.py

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cal_font.h"

static int g_checks = 0;
static int g_fails = 0;

static void Report(bool ok, const char* what, const char* detail) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  [FAIL] %s  %s\n", what, detail);
    }
}

static void ExpectInt(int got, int want, const char* what) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "got %d want %d", got, want);
    Report(got == want, what, buf);
}

static void ExpectU32(uint32_t got, uint32_t want, const char* what) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "got 0x%08X want 0x%08X",
                  static_cast<unsigned>(got), static_cast<unsigned>(want));
    Report(got == want, what, buf);
}

// ---------------------------------------------------------------- 逐字游标

struct GlyphPos {
    uint32_t cp;
    int pen;
};

struct WalkResult {
    int count;        // 返回的字形个数（缺字不在其中）
    int skipped;
    uint32_t first_skipped;
    GlyphPos glyphs[32];
};

static WalkResult Walk(int pt, const char* utf8) {
    WalkResult r = {};
    CalTextCursor cur;
    cal_text_cursor_init(pt, utf8, &cur);
    while (cal_text_cursor_next(&cur)) {
        if (r.count < 32) {
            r.glyphs[r.count].cp = cur.glyph->codepoint;
            r.glyphs[r.count].pen = cur.pen;
        }
        ++r.count;
    }
    r.skipped = cur.skipped;
    r.first_skipped = cur.first_skipped;
    return r;
}

static int LayoutWidth(int pt, const char* utf8, int* out_missing, int* out_height) {
    CalTextMetrics tm = {};
    if (!cal_text_layout(pt, utf8, CAL_ALIGN_LEFT, 0, 0, &tm)) {
        return -1;
    }
    if (out_missing != nullptr) *out_missing = tm.missing;
    if (out_height != nullptr) *out_height = tm.height;
    return tm.width;
}

// ---------------------------------------------------------------- 用例

static void CaseOverflowCharset() {
    printf("① 缺字跳过：包围盒只多出「缺字数 × 半字宽」\n");
    const char* clean = "9DP1(值班)";
    const char* dirty = "9DP1(值班早)";   // 「早」未登记进字库
    const int pt = 28;

    uint32_t first = 0;
    ExpectInt(cal_font_missing_count(pt, clean, &first), 0, "已知形态无缺字");
    ExpectInt(cal_font_missing_count(pt, dirty, &first), 1, "带「早」的串缺 1 字");
    ExpectU32(first, 0x65E9, "首个缺失码点 = U+65E9「早」");

    int miss_c = -1, miss_d = -1, h_c = 0, h_d = 0;
    const int w_c = LayoutWidth(pt, clean, &miss_c, &h_c);
    const int w_d = LayoutWidth(pt, dirty, &miss_d, &h_d);
    ExpectInt(miss_c, 0, "干净串 layout.missing");
    ExpectInt(miss_d, 1, "带缺字串 layout.missing");
    ExpectInt(w_c, 150, "9DP1(值班) @pt28 宽（PC 侧实测值）");
    ExpectInt(h_c, 30, "9DP1(值班) @pt28 高");
    ExpectInt(h_d, 30, "带缺字串高度不受影响");
    ExpectInt(w_d, w_c + CAL_MISSING_ADVANCE_PX(pt),
              "宽度增量 == CAL_MISSING_ADVANCE_PX(pt) = pt/2");
    ExpectInt(CAL_MISSING_ADVANCE_PX(pt), 14, "pt28 的半字宽");

    printf("     9DP1(值班)   -> %dx%d, missing=%d\n", w_c, h_c, miss_c);
    printf("     9DP1(值班早) -> %dx%d, missing=%d  (+%d = 跳过「早」的半字宽)\n",
           w_d, h_d, miss_d, w_d - w_c);
}

static void CaseCursorPen() {
    printf("\n② 逐字游标：缺字不进遍历，但后面的字整体右移半字宽\n");
    const int pt = 28;
    const char* clean = "9DP1(值班)";
    const char* dirty = "9DP1(值班早)";
    const int gap = CAL_MISSING_ADVANCE_PX(pt);

    const WalkResult a = Walk(pt, clean);
    const WalkResult b = Walk(pt, dirty);

    ExpectInt(a.count, 8, "干净串返回 8 个字形（含括号）");
    ExpectInt(b.count, 8, "带缺字串返回同样 8 个字形（缺字被越过，不计入）");
    ExpectInt(a.skipped, 0, "干净串 skipped");
    ExpectInt(b.skipped, 1, "带缺字串 skipped");
    ExpectU32(b.first_skipped, 0x65E9, "带缺字串 first_skipped");

    // 缺字出现在第 7 个字符（索引 6，即「班」）之后 → 第 7 号（右括号）之后整体右移
    const int mask = 1 << 7;  // 0x80 → 只比较前 7 个（索引 0..6）
    int same_before = 0;
    for (int i = 0; i < 7; ++i) {
        if (a.glyphs[i].cp == b.glyphs[i].cp && a.glyphs[i].pen == b.glyphs[i].pen) {
            ++same_before;
        }
    }
    ExpectInt(same_before, 7, "缺字之前的 7 个字形：码点与笔位完全相同");
    (void)mask;

    ExpectInt(b.glyphs[7].pen - a.glyphs[7].pen, gap,
              "缺字之后的右括号笔位右移半字宽");
    ExpectU32(b.glyphs[7].cp, static_cast<uint32_t>(')'), "第 7 号字形仍是右括号");

    printf("     pen: ");
    for (int i = 0; i < a.count && i < 8; ++i) {
        // 非 ASCII 码点用十六进制显示 —— "%c" 会把多字节码点截断成乱码
        if (a.glyphs[i].cp < 0x80) {
            printf("%c=%d", static_cast<char>(a.glyphs[i].cp), a.glyphs[i].pen);
        } else {
            printf("U+%04X=%d", static_cast<unsigned>(a.glyphs[i].cp), a.glyphs[i].pen);
        }
        if (i + 1 < a.count) printf(" ");
    }
    printf("   ->   ");
    for (int i = 0; i < b.count && i < 8; ++i) {
        if (b.glyphs[i].cp < 0x80) {
            printf("%c=%d", static_cast<char>(b.glyphs[i].cp), b.glyphs[i].pen);
        } else {
            printf("U+%04X=%d", static_cast<unsigned>(b.glyphs[i].cp), b.glyphs[i].pen);
        }
        if (i + 1 < b.count) printf(" ");
    }
    printf("\n");
}

static void CaseDegenerate() {
    printf("\n③ 退化输入：不崩、返回 false\n");
    CalTextMetrics tm = {};

    // 整串都是缺字 → 没有任何字形进包围盒
    ExpectInt(cal_text_layout(28, "早早早", CAL_ALIGN_LEFT, 0, 0, &tm) ? 1 : 0, 0,
              "整串缺字 → layout 返回 false");
    uint32_t first = 0;
    ExpectInt(cal_font_missing_count(28, "早早早", &first), 3, "整串缺字 missing_count");
    ExpectU32(first, 0x65E9, "整串缺字 first");

    // 字号组不存在（版式写错字号）
    ExpectInt(cal_text_layout(99, "9DP1", CAL_ALIGN_LEFT, 0, 0, &tm) ? 1 : 0, 0,
              "字号组不存在 → layout 返回 false");
    ExpectInt(cal_font_missing_count(99, "9DP1", nullptr), -1,
              "字号组不存在 → missing_count 返回 -1");
    CalTextCursor cur;
    cal_text_cursor_init(99, "9DP1", &cur);
    ExpectInt(cal_text_cursor_next(&cur) ? 1 : 0, 0,
              "字号组不存在 → cursor_next 返回 false 且不终止进程");

    // 空串
    ExpectInt(cal_text_layout(28, "", CAL_ALIGN_LEFT, 0, 0, &tm) ? 1 : 0, 0,
              "空串 → layout 返回 false");

    // 非法 UTF-8：脏字节只告警 + 跳过 1 字节，**不再 abort**
    char bad[] = {'9', 'D', '(', static_cast<char>(0xE5), static_cast<char>(0x80),
                  ')', '\0'};
    first = 0;
    const int n = cal_font_missing_count(28, bad, &first);
    ExpectInt(n, 2, "截断的 3 字节序列 → 2 个脏字节计入缺失");
    ExpectU32(first, 0xFFFFFFFFu, "首个缺失 = 非法字节哨兵值");
    const int w = LayoutWidth(28, bad, nullptr, nullptr);
    ExpectInt(w > 0 ? 1 : 0, 1, "含脏字节的串仍能算出包围盒（不终止进程）");
    printf("     非法字节串 -> 宽 %d，缺 %d 字节（未终止进程）\n", w, n);
}

static void CaseKnownForms() {
    printf("\n④ 已知形态全字号无缺字（与 tools/pt_census.py 的形态表同源）\n");
    static const char* kForms[] = {
        "休", "休息", "9DP1", "9NP1", "9DP(绿)", "9DP1(绿)",
        "9NP1(绿)", "9DP0(验车)", "9DP1(值班)", "9DP0(值班)",
    };
    static const int kPts[] = {20, 21, 22, 23, 24, 25, 26, 27, 28};
    int bad = 0;
    int total = 0;
    for (int pt : kPts) {
        for (const char* f : kForms) {
            ++total;
            uint32_t first = 0;
            const int m = cal_font_missing_count(pt, f, &first);
            if (m != 0) {
                ++bad;
                printf("  [FAIL] pt%d %s -> missing=%d (首个 U+%04X)\n", pt, f, m,
                       static_cast<unsigned>(first));
            }
        }
    }
    ExpectInt(bad, 0, "10 形态 × 9 字号 全部零缺字");
    printf("     %d 项检查，%d 项缺字\n", total, bad);

    // 负例：未登记的词**必须**被检出，否则守卫形同虚设
    int detected = 0;
    for (int pt : kPts) {
        if (cal_font_missing_count(pt, "9DP1(值班早)", nullptr) > 0) {
            ++detected;
        }
    }
    ExpectInt(detected, 9, "未登记的「早」在 9 个字号上全被检出（负例守卫有效）");
    printf("     负例 9DP1(值班早)：%d/9 个字号报缺字\n", detected);
}

// ---------------------------------------------------------------- 选字号

struct PickCase {
    const char* text;
    int target_h;
    int want_pt;
};

// 期望值 = PC 侧 `Canvas.font_for_height()`（二分 6..400）的实测结果，
// 也就是 tools/pt_census.py 普查时给每个区域定下的 pt。
static const PickCase kPickCases[] = {
    {"2026年9月23日", 27, 28},        // 顶栏年月日
    {"八月十三", 21, 21},             // 顶栏农历（无节日）
    {"八月十五", 18, 18},             // 顶栏农历 + 节日签
    {"腊月初八", 18, 17},
    {"七月十五", 15, 15},
    {"获取失败", 30, 30},             // 主区占位（大字）
    {"获取失败", 18, 17},             // 主区占位（小字）
    {"上午08:00-12:00", 30, 31},      // 主区时段
    {"下午14:00-18:00", 30, 32},
    {"夜间20:00-24:00", 28, 28},
};

// 失败时把每一步摊开 —— 这个 bug 上一次就是因为日志不全而难定位。
static void DumpGroups(const char* text, int target_h) {
    printf("      逐组明细（pt / 可布局 / missing / height）:\n");
    for (size_t i = 0; i < kCalFontGroupCount; ++i) {
        const int pt = kCalFontGroups[i].pt;
        CalTextMetrics tm = {};
        const bool ok = cal_text_layout(pt, text, CAL_ALIGN_LEFT, 0, 0, &tm);
        printf("        pt%-3d ok=%d missing=%-3d h=%-4d%s\n", pt, ok ? 1 : 0,
               ok ? tm.missing : -1, ok ? tm.height : -1,
               (ok && tm.missing == 0 && tm.height <= target_h) ? "  <== 候选" : "");
    }
}

static void CasePickPt() {
    printf("\n⑤ 选字号：取「最大 + 整串无缺字」的满足 pt（与 PC 侧二分等价）\n");
    const int n = static_cast<int>(sizeof(kPickCases) / sizeof(kPickCases[0]));
    int bad = 0;
    for (const PickCase& c : kPickCases) {
        const int got = cal_font_pick_pt(c.text, c.target_h);
        ++g_checks;
        if (got != c.want_pt) {
            ++g_fails;
            ++bad;
            printf("  [FAIL] %s h=%d got pt%d want pt%d\n", c.text, c.target_h, got,
                   c.want_pt);
            DumpGroups(c.text, c.target_h);
        }
    }
    ExpectInt(bad, 0, "10 例选字号全部与 PC 侧一致");
    printf("     %d 例，%d 例不符\n", n, bad);

    // 负例：不能拿一个「画不全」的字号糊弄过去
    ExpectInt(cal_font_pick_pt("早早早", 30), -1, "整串缺字 → -1");
    ExpectInt(cal_font_pick_pt("9DP1(值班早)", 30), -1,
              "部分缺字（「早」未登记）→ 任何字号都画不全，返回 -1");
    ExpectInt(cal_font_pick_pt("", 30), -1, "空串 → -1");
    ExpectInt(cal_font_pick_pt(nullptr, 30), -1, "NULL → -1");

    // 「小小目标高度」：没有字号能小到满足 → -1（而不是顶格返回最小 pt）
    ExpectInt(cal_font_pick_pt("八月十三", 3), -1, "目标高过小 → -1");

    // ⚠️ 回归点：曾经以为 kCalFontGroups 是降序、取「第一个满足的」，结果返回最小 pt。
    // 这里显式断言「取到的是最大满足 pt」，与组的排列顺序无关。
    int max_ok = -1;
    for (size_t i = 0; i < kCalFontGroupCount; ++i) {
        const int pt = kCalFontGroups[i].pt;
        CalTextMetrics tm = {};
        if (cal_text_layout(pt, "八月十三", CAL_ALIGN_LEFT, 0, 0, &tm) &&
            tm.missing == 0 && tm.height <= 21 && pt > max_ok) {
            max_ok = pt;
        }
    }
    ExpectInt(cal_font_pick_pt("八月十三", 21), max_ok, "结果 == 逐组枚举出的最大满足 pt");
    printf("     八月十三 @h21：逐组枚举最大满足 = pt%d（不是 pt14）\n", max_ok);
}

int main() {
    printf("cal_font 组件宿主侧验证（缺字跳过 + 选字号）\n");
    printf("================================================================\n\n");
    CaseOverflowCharset();
    CaseCursorPen();
    CaseDegenerate();
    CaseKnownForms();
    CasePickPt();
    printf("\n================================================================\n");
    printf("%d 项断言，%d 项失败\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
