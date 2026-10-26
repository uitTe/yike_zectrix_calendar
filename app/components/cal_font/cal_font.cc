#include "cal_font.h"

#include <climits>

#include "esp_log.h"

namespace {

constexpr char kTag[] = "cal_font";

// UTF-8 解码失败的哨兵值。字库全部在 BMP 内，0xFFFFFFFF 不可能是合法字形码点。
constexpr uint32_t kBadCodepoint = 0xFFFFFFFFu;

// 解码一个 UTF-8 码点并前移游标。
//
// ⚠️ 串的内容可以来自网络（班次名），所以**任何字节都不允许让设备终止**：
// 非法首字节或续字节只记一条告警、前进一个字节重新同步、返回 kBadCodepoint，
// 由调用方当作「缺字」跳过。早期版本这里用的是 abort() —— 那会把一次脏数据
// 变成当天不再刷新（设备一天只醒一次），代价远大于画面上多一个空档。
uint32_t Utf8Next(const char** cursor) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(*cursor);
    uint32_t cp = 0;
    int extra = 0;
    if (p[0] < 0x80) {
        cp = p[0];
        extra = 0;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07;
        extra = 3;
    } else {
        ESP_LOGW(kTag, "非法 UTF-8 首字节 0x%02X（%s），跳过 1 字节",
                 static_cast<unsigned>(p[0]), *cursor);
        *cursor += 1;
        return kBadCodepoint;
    }
    for (int i = 1; i <= extra; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            ESP_LOGW(kTag, "非法 UTF-8 续字节 0x%02X（%s），跳过 1 字节",
                     static_cast<unsigned>(p[i]), *cursor);
            *cursor += 1;
            return kBadCodepoint;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *cursor += extra + 1;
    return cp;
}

}  // namespace

extern "C" const CalFontGroup* cal_font_group(int pt) {
    for (size_t i = 0; i < kCalFontGroupCount; ++i) {
        if (kCalFontGroups[i].pt == pt) {
            return &kCalFontGroups[i];
        }
    }
    return nullptr;
}

extern "C" const CalGlyph* cal_font_lookup(int pt, uint32_t codepoint) {
    const CalFontGroup* group = cal_font_group(pt);
    if (group == nullptr) {
        return nullptr;
    }
    int low = 0;
    int high = static_cast<int>(group->count) - 1;
    while (low <= high) {
        const int mid = (low + high) / 2;
        const uint16_t value = group->glyphs[mid].codepoint;
        if (value == codepoint) {
            return &group->glyphs[mid];
        }
        if (value < codepoint) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return nullptr;
}

extern "C" bool cal_glyph_pixel(const CalFontGroup* group,
                                const CalGlyph* glyph, int row, int column) {
    if (group == nullptr || glyph == nullptr || row < 0 || column < 0 ||
        row >= glyph->ink_h || column >= glyph->ink_w) {
        return false;
    }
    const int stride = (glyph->ink_w + 7) / 8;
    const uint8_t* line = group->bitmap + glyph->offset + row * stride;
    return (line[column / 8] & (0x80 >> (column % 8))) != 0;
}

namespace {

// 布局内核（**静默**）：不打任何日志。
//
// 选字号要逐组试算，若内核自带告警，一次选字号就会刷出十几条「缺 N 个字」，
// 反而把真正的问题埋掉 —— 板端日志里正是这种现象（25 条 W 只见到 3 条）。
// 告警交给 cal_text_layout()，只在真正绘制的那一次打。
bool LayoutRaw(int pt, const char* utf8, CalAlign align, int cx, int cy,
               CalTextMetrics* out, uint32_t* out_first_missing) {
    const char* cursor = utf8;
    int pen = 0;
    int missing = 0;
    uint32_t first_missing = 0;
    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;
    while (*cursor != '\0') {
        const uint32_t cp = Utf8Next(&cursor);
        const CalGlyph* glyph = cal_font_lookup(pt, cp);
        if (glyph == nullptr) {
            // 缺字：不画、不进包围盒，但笔位要前进半字宽，好让后面的字排在
            // 正确的位置上（见 CAL_MISSING_ADVANCE_PX）。
            if (missing == 0) {
                first_missing = cp;
            }
            ++missing;
            pen += CAL_MISSING_ADVANCE_PX(pt);
            continue;
        }
        if (glyph->bbox_w > 0 && glyph->bbox_h > 0) {
            const int glyph_left = pen + glyph->bbox_dx;
            const int glyph_right = glyph_left + glyph->bbox_w;
            const int glyph_top = glyph->bbox_dy;
            const int glyph_bottom = glyph_top + glyph->bbox_h;
            if (glyph_left < left) left = glyph_left;
            if (glyph_top < top) top = glyph_top;
            if (glyph_right > right) right = glyph_right;
            if (glyph_bottom > bottom) bottom = glyph_bottom;
        }
        pen += glyph->advance;
    }
    if (out_first_missing != nullptr) {
        *out_first_missing = first_missing;
    }
    // 即便下面返回 false 也回填 missing —— 调用方靠它区分「全缺字」和「串里没有
    // 可画字形」两种成因。
    out->missing = missing;
    if (left == INT_MAX) {
        return false;
    }
    const int width = right - left;
    const int height = bottom - top;
    int x = cx;
    if (align == CAL_ALIGN_CENTER) {
        x = cx - width / 2;
    } else if (align == CAL_ALIGN_RIGHT) {
        x = cx - width;
    }
    out->width = width;
    out->height = height;
    out->origin_x = x - left;
    out->origin_y = cy - height / 2 - top;
    return true;
}

}  // namespace

extern "C" bool cal_text_layout(int pt, const char* utf8, CalAlign align, int cx,
                                int cy, CalTextMetrics* out) {
    if (out == nullptr || utf8 == nullptr || utf8[0] == '\0') {
        return false;
    }
    if (cal_font_group(pt) == nullptr) {
        // 与「缺字」区分开：字号组不存在是**调用方的版式 bug**，不是内容问题。
        ESP_LOGE(kTag, "字库里没有字号 pt%d，本串跳过不画", pt);
        return false;
    }
    uint32_t first_missing = 0;
    if (!LayoutRaw(pt, utf8, align, cx, cy, out, &first_missing)) {
        if (out->missing > 0) {
            ESP_LOGW(kTag, "pt%d 缺 %d 个字（首个 U+%04X），整串画不出：%s", pt,
                     out->missing, static_cast<unsigned>(first_missing), utf8);
        }
        return false;
    }
    if (out->missing > 0) {
        // 首个缺失码点打印为 FFFFFFFF 时表示那一位是非法 UTF-8 字节（哨兵值）。
        ESP_LOGW(kTag, "pt%d 缺 %d 个字（首个 U+%04X），已按 B 方案跳过不画：%s",
                 pt, out->missing, static_cast<unsigned>(first_missing), utf8);
    }
    return true;
}

extern "C" int cal_font_pick_pt(const char* utf8, int target_h) {
    if (utf8 == nullptr || utf8[0] == '\0') {
        return -1;
    }
    int best = -1;
    for (size_t i = 0; i < kCalFontGroupCount; ++i) {
        const int pt = kCalFontGroups[i].pt;
        if (pt <= best) {
            continue;  // 已经有更好的，跳过（也让本函数不依赖组的排列顺序）
        }
        CalTextMetrics tm = {};
        if (!LayoutRaw(pt, utf8, CAL_ALIGN_LEFT, 0, 0, &tm, nullptr)) {
            continue;
        }
        // 两条判据缺一不可（2026-10-14 板端自检抓出的真 bug）：
        //   missing == 0 —— 画不全的字号不参与选择，否则缺字掏空包围盒后高度虚小；
        //   取**最大**     —— 上一版误以为组是降序、取「第一个满足的」，结果退化成最小 pt。
        if (tm.missing == 0 && tm.height <= target_h) {
            best = pt;
        }
    }
    return best;
}

extern "C" void cal_text_cursor_init(int pt, const char* utf8,
                                     CalTextCursor* cursor) {
    if (cursor == nullptr) {
        return;
    }
    cursor->group = cal_font_group(pt);
    cursor->cursor = utf8;
    cursor->glyph = nullptr;
    cursor->pen = 0;
    cursor->next_pen = 0;
    cursor->skipped = 0;
    cursor->first_skipped = 0;
    if (cursor->group == nullptr) {
        // 字号组不存在是**调用方的版式 bug**，但也不在绘制路径上终止进程 ——
        // 记错误、让 next() 恒返回 false（该区域画不出东西），
        // 由 app_main 的启动自检把这个错误大声报出来。
        ESP_LOGE(kTag, "字库里没有字号 pt%d，本串跳过不画", pt);
    }
}

extern "C" bool cal_text_cursor_next(CalTextCursor* cursor) {
    if (cursor == nullptr || cursor->group == nullptr ||
        cursor->cursor == nullptr) {
        return false;
    }
    while (cursor->cursor[0] != '\0') {
        const uint32_t cp = Utf8Next(&cursor->cursor);
        const CalGlyph* glyph = cal_font_lookup(cursor->group->pt, cp);
        if (glyph != nullptr) {
            cursor->glyph = glyph;
            cursor->pen = cursor->next_pen;
            cursor->next_pen += glyph->advance;
            return true;
        }
        // 缺字：越过它，但让笔位跳过对应的半字宽缺口 —— 绘制循环因此可以
        // 无条件地「有 glyph 就在 pen 处贴图」，不必自己处理空档。
        if (cursor->skipped == 0) {
            cursor->first_skipped = cp;
        }
        ++cursor->skipped;
        cursor->next_pen += CAL_MISSING_ADVANCE_PX(cursor->group->pt);
    }
    cursor->glyph = nullptr;  // 别让调用方拿到上一轮的字形
    return false;
}

extern "C" int cal_font_missing_count(int pt, const char* utf8,
                                      uint32_t* out_first_missing) {
    if (cal_font_group(pt) == nullptr) {
        return -1;
    }
    if (utf8 == nullptr) {
        return 0;
    }
    if (out_first_missing != nullptr) {
        *out_first_missing = 0;
    }
    int missing = 0;
    const char* cursor = utf8;
    while (*cursor != '\0') {
        // Utf8Next 对非法 UTF-8 只告警 + 前进 1 字节并返回哨兵值，不再终止进程；
        // 这里把「字形查不到」和「字节非法」一并算作缺失字符。
        const uint32_t cp = Utf8Next(&cursor);
        if (cp == kBadCodepoint || cal_font_lookup(pt, cp) == nullptr) {
            if (missing == 0 && out_first_missing != nullptr) {
                *out_first_missing = cp;
            }
            ++missing;
        }
    }
    return missing;
}
