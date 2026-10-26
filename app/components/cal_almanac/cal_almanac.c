/* 历法：农历 / 节气 / 节日 —— 纯 C99 实现。
 *
 * 对照实现：tools/lunar_cn.py（农历 + 节日，源 lunardate 0.3.0）
 *           tools/solar_terms.py（24 节气，Meeus 低精度天文法，零数据表）
 *
 * 本文件刻意**不 include 任何 ESP-IDF 头**：同一份源码在 PC 上用宿主 gcc 编译，
 * 跑 tools/almanac_test.py 做逐日黄金向量比对 —— 板端与 PC 端结果必须逐字节相同。
 */
#include "cal_almanac.h"
#include "cal_lunar_table.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ================================================================ 中文名表 */

static const char WEEKDAY_CN[7][4] = {"一", "二", "三", "四", "五", "六", "日"};

static const char LUNAR_MONTH_CN[12][4] = {
    "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊",
};

/* 下标 1..30；下标 0 是占位（农历没有初零） */
static const char LUNAR_DAY_CN[31][8] = {
    "",   "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九",
    "初十", "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九",
    "二十", "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九",
    "三十",
};

/* ================================================================ 公历工具 */

/* Howard Hinnant 的 days_from_civil / civil_from_days（前推格里高利历）。
 * 返回「自 1970-01-01 起的天数」，1900–2099 全域精确。 */
static int32_t days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int32_t)era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int *y, int *m, int *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned dd = doy - (153u * mp + 2u) / 5u + 1u;
    int mm = (int)mp + ((mp < 10u) ? 3 : -9); /* 有符号：mp<10 时是 +3，否则 -9 */
    int yr = (int)yoe + (int)era * 400 + (mm <= 2 ? 1 : 0);
    *y = yr;
    *m = mm;
    *d = (int)dd;
}

static int32_t date_days(const cal_date *d)
{
    return days_from_civil(d->year, d->month, d->day);
}

static void days_to_date(int32_t z, cal_date *out)
{
    civil_from_days(z, &out->year, &out->month, &out->day);
}

/* 0 = 周一：1970-01-01 是周四 → +3 天偏移 */
static int weekday_of_days(int32_t z)
{
    int wd = (int)((z + 3) % 7);
    return (wd < 0) ? wd + 7 : wd;
}

int cal_weekday(int year, int month, int day)
{
    return weekday_of_days(days_from_civil(year, month, day));
}

bool cal_is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int cal_days_in_month(int year, int month)
{
    static const int MDAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && cal_is_leap_year(year)) return 29;
    return MDAYS[month - 1];
}

/* 「某年某月第 n 个星期 weekday」的天数（weekday 0 = 周一）。 */
static int32_t nth_weekday_days(int year, int month, int weekday, int n)
{
    int32_t first = days_from_civil(year, month, 1);
    int off = ((weekday - weekday_of_days(first)) % 7 + 7) % 7;
    return first + off + (int32_t)(n - 1) * 7;
}

/* ================================================================ 农历解码 */

/* 说明：年表编码以 lunardate 源码为准 —— bit15 = 正月、bit4 = 腊月（MSB→正月）。
 * 曾经的笔记写成「LSB→正月」，是错的；见 tools/gen_lunar_table.py 头部注释。 */

int cal_lunar_leap_month(int lunar_year)
{
    if (lunar_year < CAL_LUNAR_FIRST_YEAR || lunar_year > CAL_LUNAR_LAST_YEAR) return 0;
    return (int)(cal_lunar_info[lunar_year - CAL_LUNAR_FIRST_YEAR] & 0x0Fu);
}

int cal_lunar_month_days(int lunar_year, int month)
{
    if (lunar_year < CAL_LUNAR_FIRST_YEAR || lunar_year > CAL_LUNAR_LAST_YEAR) return 0;
    if (month < 1 || month > 12) return 0;
    return ((cal_lunar_info[lunar_year - CAL_LUNAR_FIRST_YEAR] >> (16 - month)) & 1u) ? 30 : 29;
}

int cal_lunar_leap_days(int lunar_year)
{
    if (cal_lunar_leap_month(lunar_year) == 0) return 0;
    return ((cal_lunar_info[lunar_year - CAL_LUNAR_FIRST_YEAR] >> 16) & 1u) ? 30 : 29;
}

int cal_lunar_year_days(int lunar_year)
{
    if (lunar_year < CAL_LUNAR_FIRST_YEAR || lunar_year > CAL_LUNAR_LAST_YEAR) return 0;
    uint32_t info = cal_lunar_info[lunar_year - CAL_LUNAR_FIRST_YEAR];
    int res = 348;                       /* 12 × 29 */
    int leap = (int)(info & 0x0Fu);
    int total = 12;
    if (leap) { res += 29; total = 13; } /* 闰月先按 29 天计 */
    info >>= 4;                          /* 对齐到 bit4 = 腊月 */
    for (int i = 0; i < total; i++) {    /* 第 13 位即原 bit16 = 闰月月长 */
        if (info & 1u) res += 1;
        info >>= 1;
    }
    return res;
}

/* 农历 1900 年正月初一对应的公历日 = 1900-01-31 */
#define CAL_LUNAR_EPOCH_DAYS (days_from_civil(1900, 1, 31))

bool cal_lunar_from_solar(const cal_date *d, cal_lunar *out)
{
    if (!d || !out) return false;
    if (d->year < CAL_LUNAR_FIRST_YEAR || d->year > CAL_LUNAR_LAST_YEAR) return false;

    int32_t offset = date_days(d) - CAL_LUNAR_EPOCH_DAYS;
    if (offset < 0) return false;

    int year = CAL_LUNAR_FIRST_YEAR;
    for (;;) {
        if (year > CAL_LUNAR_LAST_YEAR) return false;
        int yd = cal_lunar_year_days(year);
        if (offset < yd) break;
        offset -= yd;
        year++;
    }

    int leap = cal_lunar_leap_month(year);
    int month = 1;
    bool leap_done = false;
    for (int guard = 0; guard < 16; guard++) {
        /* 闰月紧随同名正常月之后：处理完 month==leap 后，先插闰月再走 leap+1 */
        bool cur_leap = (leap > 0 && !leap_done && month == leap + 1);
        int len = cur_leap ? cal_lunar_leap_days(year)
                           : cal_lunar_month_days(year, month);
        if (offset < len) {
            out->year = year;
            out->month = cur_leap ? leap : month;
            out->day = (int)offset + 1;
            out->is_leap = cur_leap;
            return true;
        }
        offset -= len;
        if (cur_leap) {
            /* 闰月紧跟同名正常月：闰月被处理时 month 已经是 leap+1，
             * 所以此处**保持 month 不变**，下一轮直接走正常月 leap+1。 */
            leap_done = true;
        } else {
            month++;
        }
    }
    return false; /* 不可达：一年最多 13 个月 */
}

bool cal_lunar_to_solar(const cal_lunar *l, cal_date *out)
{
    if (!l || !out) return false;
    if (l->year < CAL_LUNAR_FIRST_YEAR || l->year > CAL_LUNAR_LAST_YEAR) return false;
    if (l->month < 1 || l->month > 12) return false;

    int leap = cal_lunar_leap_month(l->year);
    if (l->is_leap && leap != l->month) return false;

    int32_t offset = 0;
    for (int y = CAL_LUNAR_FIRST_YEAR; y < l->year; y++) offset += cal_lunar_year_days(y);

    int month = 1;
    bool leap_done = false;
    bool found = false;
    for (int guard = 0; guard < 16 && !found; guard++) {
        bool cur_leap = (leap > 0 && !leap_done && month == leap + 1);
        int len = cur_leap ? cal_lunar_leap_days(l->year)
                           : cal_lunar_month_days(l->year, month);
        int mm = cur_leap ? leap : month;
        if (mm == l->month && cur_leap == l->is_leap) {
            if (l->day < 1 || l->day > len) return false;
            offset += l->day - 1;
            found = true;
            break;
        }
        offset += len;
        if (cur_leap) {
            leap_done = true; /* 同上：month 已是 leap+1，不再自增 */
        } else {
            month++;
        }
    }
    if (!found) return false;

    days_to_date(CAL_LUNAR_EPOCH_DAYS + offset, out);
    return true;
}

void cal_lunar_text(const cal_lunar *l, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    buf[0] = '\0';
    if (!l) return;
    if (l->month < 1 || l->month > 12 || l->day < 1 || l->day > 30) return;
    snprintf(buf, n, "%s%s月%s", l->is_leap ? "闰" : "",
             LUNAR_MONTH_CN[l->month - 1], LUNAR_DAY_CN[l->day]);
}

bool cal_is_lunar_new_year_eve(const cal_date *d)
{
    cal_date nxt;
    days_to_date(date_days(d) + 1, &nxt);
    cal_lunar lu;
    if (!cal_lunar_from_solar(&nxt, &lu)) return false;
    return lu.month == 1 && lu.day == 1 && !lu.is_leap;
}

/* ================================================================ 节日 */

typedef struct {
    int m, d;
    const char *name;
} fest_fixed;

/* 公历固定节日（14） */
static const fest_fixed FEST_SOLAR[] = {
    {1, 1, "元旦"},     {2, 14, "情人节"}, {3, 8, "妇女节"},  {3, 12, "植树节"},
    {4, 1, "愚人节"},   {5, 1, "劳动节"},  {5, 4, "青年节"},  {6, 1, "儿童节"},
    {7, 1, "建党节"},   {8, 1, "建军节"},  {9, 10, "教师节"}, {10, 1, "国庆节"},
    {12, 24, "平安夜"}, {12, 25, "圣诞节"},
};

/* 农历固定节日（10）；闰月不过节 */
static const fest_fixed FEST_LUNAR[] = {
    {1, 1, "春节"},    {1, 15, "元宵节"}, {2, 2, "龙抬头"}, {5, 5, "端午节"},
    {7, 7, "七夕节"},  {7, 15, "中元节"}, {8, 15, "中秋节"}, {9, 9, "重阳节"},
    {12, 8, "腊八节"}, {12, 23, "小年"},
};

/* 重要性；未登记的洋节 / 纪念日按 0 计（与 Python `FEST_PRIORITY.get(n, 0)` 一致） */
static const struct {
    const char *name;
    int prio;
} FEST_PRIORITY[] = {
    {"春节", 100},  {"除夕", 100},  {"中秋节", 95}, {"端午节", 90}, {"国庆节", 88},
    {"劳动节", 86}, {"元旦", 85},   {"元宵节", 80}, {"重阳节", 75}, {"七夕节", 70},
    {"儿童节", 70}, {"教师节", 68}, {"腊八节", 65}, {"小年", 60},   {"母亲节", 60},
    {"父亲节", 60}, {"感恩节", 55},
};

static int fest_priority(const char *name)
{
    for (size_t i = 0; i < sizeof(FEST_PRIORITY) / sizeof(FEST_PRIORITY[0]); i++) {
        if (strcmp(FEST_PRIORITY[i].name, name) == 0) return FEST_PRIORITY[i].prio;
    }
    return 0;
}

static const char *fest_lookup_lunar(int month, int day)
{
    for (size_t i = 0; i < sizeof(FEST_LUNAR) / sizeof(FEST_LUNAR[0]); i++) {
        if (FEST_LUNAR[i].m == month && FEST_LUNAR[i].d == day) return FEST_LUNAR[i].name;
    }
    return NULL;
}

static const char *fest_lookup_solar(int month, int day)
{
    for (size_t i = 0; i < sizeof(FEST_SOLAR) / sizeof(FEST_SOLAR[0]); i++) {
        if (FEST_SOLAR[i].m == month && FEST_SOLAR[i].d == day) return FEST_SOLAR[i].name;
    }
    return NULL;
}

int cal_festivals(const cal_date *d, char out[CAL_FEST_MAX][CAL_FEST_NAME_MAX])
{
    if (!d || !out) return 0;

    /* 探测顺序（先除夕 → 农历节 → 公历节 → 规则日）必须与 lunar_cn.festivals() 一致：
     * 排序是稳定的，同分时靠这个顺序决定先后。 */
    struct {
        const char *name;
        int prio;
    } hit[6];
    int n = 0;

    if (cal_is_lunar_new_year_eve(d)) {
        hit[n].name = "除夕";
        hit[n++].prio = fest_priority("除夕");
    }

    cal_lunar lu;
    if (cal_lunar_from_solar(d, &lu) && !lu.is_leap) { /* 闰月不过节 */
        const char *nm = fest_lookup_lunar(lu.month, lu.day);
        if (nm) {
            hit[n].name = nm;
            hit[n++].prio = fest_priority(nm);
        }
    }

    const char *nm = fest_lookup_solar(d->month, d->day);
    if (nm) {
        hit[n].name = nm;
        hit[n++].prio = fest_priority(nm);
    }

    /* 规则日：母亲节 5 月第 2 个周日 / 父亲节 6 月第 3 个周日 / 感恩节 11 月第 4 个周四 */
    int32_t z = date_days(d);
    if (z == nth_weekday_days(d->year, 5, 6, 2)) {
        hit[n].name = "母亲节";
        hit[n++].prio = fest_priority("母亲节");
    }
    if (z == nth_weekday_days(d->year, 6, 6, 3)) {
        hit[n].name = "父亲节";
        hit[n++].prio = fest_priority("父亲节");
    }
    if (z == nth_weekday_days(d->year, 11, 3, 4)) {
        hit[n].name = "感恩节";
        hit[n++].prio = fest_priority("感恩节");
    }

    /* 插入排序，**严格小于**才交换 → 稳定，同分保持探测顺序 */
    for (int i = 1; i < n; i++) {
        const char *kn = hit[i].name;
        int kp = hit[i].prio;
        int j = i - 1;
        while (j >= 0 && hit[j].prio < kp) {
            hit[j + 1] = hit[j];
            j--;
        }
        hit[j + 1].name = kn;
        hit[j + 1].prio = kp;
    }

    int m = (n < CAL_FEST_MAX) ? n : CAL_FEST_MAX;
    for (int i = 0; i < m; i++) {
        snprintf(out[i], CAL_FEST_NAME_MAX, "%s", hit[i].name);
    }
    return m;
}

/* ================================================================ 节气
 *
 * Meeus《Astronomical Algorithms》第 25 章低精度太阳视黄经：
 *   L0（平黄经）+ C（中心差）+ 章动修正 - 光行差常数，再二分求「黄经 = 目标角」的时刻。
 * 与公开时刻差 5–10 分钟 → **日期判定 100% 可靠**；无数据表，任意年份通用。
 * ⚠️ 不要换回「寿星公式」：去掉例外表后 2021 年冬至会错一天。
 */

#define CAL_DEG2RAD (0.017453292519943295769236907684886)

static const struct {
    const char *name;
    double lon; /* 目标视黄经（度） */
    int sm, sd; /* 搜索起点（公历月、日） */
} TERMS[CAL_TERM_NUM] = {
    {"小寒", 285, 1, 1},   {"大寒", 300, 1, 15},  {"立春", 315, 2, 1},
    {"雨水", 330, 2, 15},  {"惊蛰", 345, 3, 1},   {"春分", 0, 3, 15},
    {"清明", 15, 4, 1},    {"谷雨", 30, 4, 15},   {"立夏", 45, 5, 1},
    {"小满", 60, 5, 16},   {"芒种", 75, 6, 1},    {"夏至", 90, 6, 16},
    {"小暑", 105, 7, 1},   {"大暑", 120, 7, 16},  {"立秋", 135, 8, 1},
    {"处暑", 150, 8, 16},  {"白露", 165, 9, 1},   {"秋分", 180, 9, 16},
    {"寒露", 195, 10, 1},  {"霜降", 210, 10, 16}, {"立冬", 225, 11, 1},
    {"小雪", 240, 11, 15}, {"大雪", 255, 12, 1},  {"冬至", 270, 12, 15},
};

#define CAL_JD_UNIX_EPOCH 2440587.5 /* 1970-01-01 00:00 UTC 的儒略日 */

static double sun_apparent_lon(double jd)
{
    double t = (jd - 2451545.0) / 36525.0;
    double l0 = 280.46646 + 36000.76983 * t + 0.0003032 * t * t;
    double m = (357.52911 + 35999.05029 * t - 0.0001537 * t * t) * CAL_DEG2RAD;
    double c = (1.914602 - 0.004817 * t - 0.000014 * t * t) * sin(m) +
               (0.019993 - 0.000101 * t) * sin(2.0 * m) +
               0.000289 * sin(3.0 * m);
    double omega = (125.04 - 1934.136 * t) * CAL_DEG2RAD;
    double v = l0 + c - 0.00569 - 0.00478 * sin(omega);
    v = fmod(v, 360.0);
    return (v < 0.0) ? v + 360.0 : v;
}

/* 视黄经与目标角的差，归一到 (-180, 180] */
static double term_rel(double jd, double target)
{
    double diff = sun_apparent_lon(jd) - target;
    while (diff < -180.0) diff += 360.0;
    while (diff > 180.0) diff -= 360.0;
    return diff;
}

/* 求某节气的儒略日（UTC）。失败返回 -1。 */
static double solve_term_jd(int year, int idx)
{
    double target = TERMS[idx].lon;
    double lo = CAL_JD_UNIX_EPOCH + (double)days_from_civil(year, TERMS[idx].sm, TERMS[idx].sd);
    const double step = 0.25; /* 6 小时 */
    double hi = lo + step;

    int k = 0;
    while (!(term_rel(lo, target) <= 0.0 && term_rel(hi, target) >= 0.0)) {
        if (++k > 80) return -1.0; /* 80 × 6 h = 20 天，够覆盖最大偏移 */
        lo = hi;
        hi += step;
    }
    for (int i = 0; i < 60; i++) { /* 二分到远优于 1 秒 */
        double mid = 0.5 * (lo + hi);
        if (term_rel(mid, target) < 0.0) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

/* 节气在**北京时间**的日期（自 1970-01-01 起的天数）。失败返回 INT32_MIN。 */
static int32_t term_date_days(int year, int idx)
{
    double jd = solve_term_jd(year, idx);
    if (jd < 0.0) return INT32_MIN;
    double bj = (jd - CAL_JD_UNIX_EPOCH) + 8.0 / 24.0; /* 北京时间 = UTC+8 */
    return (int32_t)floor(bj);
}

const char *cal_solar_term_name(int idx)
{
    if (idx < 0 || idx >= CAL_TERM_NUM) return NULL;
    return TERMS[idx].name;
}

bool cal_solar_term_date(int year, int idx, cal_date *out)
{
    if (!out || idx < 0 || idx >= CAL_TERM_NUM) return false;
    int32_t z = term_date_days(year, idx);
    if (z == INT32_MIN) return false;
    days_to_date(z, out);
    return true;
}

bool cal_solar_terms_of_year(int year, cal_date out[CAL_TERM_NUM])
{
    if (!out) return false;
    for (int i = 0; i < CAL_TERM_NUM; i++) {
        if (!cal_solar_term_date(year, i, &out[i])) return false;
    }
    return true;
}

int cal_solar_term_index(const cal_date *d)
{
    if (!d) return -1;
    int32_t z = date_days(d);
    /* 24 个节气各算一次：单次约 130 次黄经求值，毫秒级；每天 00:00 只跑一遍，
     * 不做「只算本月两个」的特化 —— 完整性优先，别为省这点算力引入边界假设。 */
    for (int i = 0; i < CAL_TERM_NUM; i++) {
        if (term_date_days(d->year, i) == z) return i;
    }
    return -1;
}

/* ================================================================ 汇总 */

void cal_day_info_get(const cal_date *d, cal_day_info *out)
{
    if (!d || !out) return;
    memset(out, 0, sizeof(*out));

    out->weekday = cal_weekday(d->year, d->month, d->day);
    snprintf(out->date_cn, sizeof(out->date_cn), "%d年%d月%d日", d->year, d->month, d->day);
    snprintf(out->weekday_cn, sizeof(out->weekday_cn), "星期%s", WEEKDAY_CN[out->weekday]);

    if (cal_lunar_from_solar(d, &out->lunar)) {
        cal_lunar_text(&out->lunar, out->lunar_cn, sizeof(out->lunar_cn));
    }

    out->fest_count = cal_festivals(d, out->festivals);

    const char *term = NULL;
    int ti = cal_solar_term_index(d);
    if (ti >= 0) term = TERMS[ti].name;

    /* 清明让位：清明是唯一「节气即节日」（清明 ⊂ 清明节）。
     * 节气本身没人过，过的是「清明节」—— 没登记就补一个，顶栏写「清明节」、右栏不画节气。 */
    if (term && strcmp(term, "清明") == 0) {
        bool registered = false;
        for (int i = 0; i < out->fest_count; i++) {
            if (strstr(out->festivals[i], "清明")) registered = true;
        }
        if (!registered) {
            snprintf(out->festivals[0], CAL_FEST_NAME_MAX, "%s", "清明节");
            out->fest_count = 1;
        }
    }

    /* 节气名被某个节日名包住 → 该节气让位（否则同一件事在顶栏和右栏各写一遍） */
    if (term) {
        for (int i = 0; i < out->fest_count; i++) {
            if (strstr(out->festivals[i], term) != NULL) {
                term = NULL;
                break;
            }
        }
    }
    if (term) snprintf(out->term, sizeof(out->term), "%s", term);
    else out->term[0] = '\0';
}
