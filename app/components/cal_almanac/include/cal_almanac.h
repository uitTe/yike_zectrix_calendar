/* 历法：农历 / 节气 / 节日 —— 台历页离线信息源。
 *
 * 与 tools/lunar_cn.py + tools/solar_terms.py **严格对应**（同一张年表、同一套算法）。
 * 全部离线计算，不依赖网络、不依赖板载 RTC 以外的时间源。
 *
 * 本组件是**纯 C99**、不引任何 ESP-IDF 头 → 同一份源码能在 PC 上用宿主编译器编译，
 * 跑 tools/almanac_test.py 做黄金向量比对（见 docs/03 §2.3）。
 */
#ifndef CAL_ALMANAC_H
#define CAL_ALMANAC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 基础类型 */

typedef struct {
    int year;
    int month; /* 1..12 */
    int day;   /* 1..31 */
} cal_date;

typedef struct {
    int  year;    /* 农历年 1900..2099 */
    int  month;   /* 农历月 1..12 */
    int  day;     /* 农历日 1..30 */
    bool is_leap; /* 是否闰月 */
} cal_lunar;

#define CAL_FEST_MAX      3  /* 实测同日最多 2 个，留 1 余量 */
#define CAL_FEST_NAME_MAX 16 /* UTF-8，"中秋节" = 10 B */
#define CAL_TEXT_MAX      28 /* "2026年9月23日" = 17 B，留余量 */
#define CAL_TERM_NUM      24
#define CAL_TERM_NAME_MAX 8 /* 节气名固定 2 字 = 7 B */

/* 一天的全部离线信息（顶栏 / 主区右栏 / 节日签都用它） */
typedef struct {
    int  weekday;                                    /* 0 = 周一 … 6 = 周日 */
    char date_cn[CAL_TEXT_MAX];                      /* "2026年9月23日" */
    char weekday_cn[CAL_TEXT_MAX];                   /* "星期三" */
    cal_lunar lunar;
    char lunar_cn[CAL_TEXT_MAX];                     /* "八月十三" / "闰六月初一" */
    int  fest_count;                                 /* 0 = 当天无节日 */
    char festivals[CAL_FEST_MAX][CAL_FEST_NAME_MAX]; /* 按重要性降序；[0] 最重要 */
    char term[CAL_TERM_NAME_MAX];                    /* 当天节气名；"" = 不显示 */
} cal_day_info;

/* ---------------------------------------------------------------- 公历工具 */

/* 0 = 周一 … 6 = 周日（与 Python `date.weekday()` 一致）。 */
int cal_weekday(int year, int month, int day);
bool cal_is_leap_year(int year);
int cal_days_in_month(int year, int month);

/* ---------------------------------------------------------------- 农历 */

/* 农历年长（天）：348 + 各月多出的 1 天 + 闰月。 */
int cal_lunar_year_days(int lunar_year);
/* 闰月月份，0 = 当年无闰月。 */
int cal_lunar_leap_month(int lunar_year);
/* 农历月长（天），month 取 1..12。 */
int cal_lunar_month_days(int lunar_year, int month);
/* 闰月月长（天）；当年无闰月时返回 0。 */
int cal_lunar_leap_days(int lunar_year);

/* 公历 → 农历。超出 1900–2099 返回 false。 */
bool cal_lunar_from_solar(const cal_date *d, cal_lunar *out);
/* 农历 → 公历（自检用，固件主流程不需要）。 */
bool cal_lunar_to_solar(const cal_lunar *l, cal_date *out);
/* 农历中文串："八月十三" / "闰六月初一" / "腊月三十"。buf 建议 CAL_TEXT_MAX。 */
void cal_lunar_text(const cal_lunar *l, char *buf, size_t n);
/* 今天是否除夕（= 明天是正月初一）。比翻年表算腊月天数更稳，天然覆盖 29/30 天。 */
bool cal_is_lunar_new_year_eve(const cal_date *d);

/* ---------------------------------------------------------------- 节日 */

/* 当天命中的**全部**节日，按重要性降序写入 out，返回个数（0..CAL_FEST_MAX）。
 *
 * 同日撞车真的会发生：1900–2099 共 69 天（全是 2 个、没有 3 个），
 * 如 中秋节+国庆节(2031-10-01)、除夕+情人节(2037-02-14)、腊八节+元旦(2031-01-01)。
 * **闰月不过节**（农历节日只在正常月上判）。
 * ⚠️ 展示层必须用本函数，不能用「只取第一个」——那样会静默丢掉一个节日。
 */
int cal_festivals(const cal_date *d,
                  char out[CAL_FEST_MAX][CAL_FEST_NAME_MAX]);

/* ---------------------------------------------------------------- 节气 */

/* 第 idx 个节气名，idx 0..23（0 = 小寒 … 23 = 冬至）。 */
const char *cal_solar_term_name(int idx);
/* 某节气在指定年份的**北京时间日期**。成功返回 true。 */
bool cal_solar_term_date(int year, int idx, cal_date *out);
/* 某年 24 个节气日期，按 CAL_TERM_NUM 顺序写入 out。全部成功才返回 true。 */
bool cal_solar_terms_of_year(int year, cal_date out[CAL_TERM_NUM]);
/* 当天节气索引，不是节气日返回 -1。 */
int cal_solar_term_index(const cal_date *d);

/* ---------------------------------------------------------------- 汇总 */

/* 一天的全部离线信息。已含「清明让位」去重：
 * 清明是唯一「节气即节日」（清明 ⊂ 清明节）→ 只留节日、节气位不画。
 */
void cal_day_info_get(const cal_date *d, cal_day_info *out);

#ifdef __cplusplus
}
#endif
#endif /* CAL_ALMANAC_H */
