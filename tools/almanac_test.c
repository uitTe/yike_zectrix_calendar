/* 历法 C 实现的主机侧验证 —— 与 Python 参照实现逐项比对。
 *
 * 用法（由 tools/almanac_test.py 驱动，一般不用手动跑）：
 *   gcc -O2 -std=c11 -Iapp/components/cal_almanac/include -Itools/testdata \
 *       tools/almanac_test.c app/components/cal_almanac/cal_almanac.c \
 *       app/components/cal_almanac/cal_lunar_table.c -o almanac_test -lm
 *
 * 覆盖：
 *   ① 闰月表 / 农历年长  1900–2099 全 200 年
 *   ② 24 节气名 + 4800 个节气日期
 *   ③ 逐日全信息         5083 天（含「清明让位」后的有效值）
 *   ④ 除夕清单           199 天
 *   ⑤ 多节日撞车清单     69 天
 *   ⑥ 农历中文串         639 条
 *   ⑦ 全域自洽           1900-01-31..2099-12-31 逐日往返 + 星期交叉验算
 *   ⑧ 边界               年表上下界与越界拒绝
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "almanac_vectors.h"
#include "cal_almanac.h"
#include "cal_lunar_table.h"

static int g_checks = 0;
static int g_fails = 0;
static int g_shown = 0;

#define MAX_SHOW 12

/* 公历日推进/回退（测试自带，不借实现内部函数） */
static void day_shift(cal_date *d, int delta)
{
    static const int MDAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    while (delta > 0) {
        int dim = MDAYS[d->month - 1];
        if (d->month == 2 && cal_is_leap_year(d->year)) dim = 29;
        if (++d->day > dim) { d->day = 1; if (++d->month > 12) { d->month = 1; d->year++; } }
        delta--;
    }
    while (delta < 0) {
        if (--d->day < 1) {
            if (--d->month < 1) { d->month = 12; d->year--; }
            int dim = MDAYS[d->month - 1];
            if (d->month == 2 && cal_is_leap_year(d->year)) dim = 29;
            d->day = dim;
        }
        delta++;
    }
}

static void fail(const char *fmt, ...)
{
    g_fails++;
    if (g_shown < MAX_SHOW) {
        va_list ap;
        va_start(ap, fmt);
        printf("    ✗ ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
        g_shown++;
    } else if (g_shown == MAX_SHOW) {
        printf("    … 后续失败已省略\n");
        g_shown++;
    }
}

static void expect_i(const char *what, int got, int want, int y, int m, int d)
{
    g_checks++;
    if (got != want) fail("%s %04d-%02d-%02d: got %d, want %d", what, y, m, d, got, want);
}

static void expect_s(const char *what, const char *got, const char *want, int y, int m, int d)
{
    g_checks++;
    if (strcmp(got, want) != 0)
        fail("%s %04d-%02d-%02d: got \"%s\", want \"%s\"", what, y, m, d, got, want);
}

/* 独立于实现的星期算法（Zeller 同余），用来交叉验算 cal_weekday */
static int zeller_weekday(int y, int m, int d)
{
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100, J = y / 100;
    int h = (d + 13 * (m + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7; /* 0 = 周六 */
    /* h: 0=Sat,1=Sun,2=Mon,...  → 转 0=Mon */
    return (h + 5) % 7;
}

static void section(const char *name)
{
    printf("  · %s\n", name);
}

int main(void)
{
    cal_lunar lu;

    printf("历法 C 实现 vs Python 参照实现 —— 黄金向量比对\n");

    /* ---- ① 闰月表 ---- */
    printf("  · ① 闰月表 %d–%d\n", VEC_FIRST_YEAR, VEC_LAST_YEAR);
    for (int i = 0; i < VEC_YEARS; i++) {
        int y = VEC_FIRST_YEAR + i;
        expect_i("闰月", cal_lunar_leap_month(y), VEC_LEAP[i], y, 0, 0);
    }

    /* ---- ① 农历年长 ---- */
    section("② 农历年长（= 次年正月初一 - 本年正月初一）");
    for (int i = 0; i < VEC_YEARS; i++) {
        int y = VEC_FIRST_YEAR + i;
        if (VEC_YEARDAYS[i] < 0) continue;
        expect_i("年长", cal_lunar_year_days(y), VEC_YEARDAYS[i], y, 0, 0);
    }
    /* 年长 ≥ Σ 各月长 + 闰月长 */
    for (int y = VEC_FIRST_YEAR; y <= VEC_LAST_YEAR; y++) {
        int sum = 0;
        for (int m = 1; m <= 12; m++) sum += cal_lunar_month_days(y, m);
        sum += cal_lunar_leap_days(y);
        expect_i("年长=月长之和", cal_lunar_year_days(y), sum, y, 0, 0);
    }

    /* ---- ② 节气名 ---- */
    section("③ 24 节气名");
    for (int i = 0; i < 24; i++) {
        g_checks++;
        const char *got = cal_solar_term_name(i);
        if (!got || strcmp(got, VEC_TERM_NAMES[i]) != 0)
            fail("节气名[%d]: got \"%s\", want \"%s\"", i, got ? got : "(null)", VEC_TERM_NAMES[i]);
    }

    /* ---- ② 节气日期 ---- */
    printf("  · ④ 节气日期 %d 年 × 24 = %d\n", VEC_YEARS, VEC_YEARS * 24);
    for (int i = 0; i < VEC_YEARS; i++) {
        int y = VEC_FIRST_YEAR + i;
        cal_date got[24];
        if (!cal_solar_terms_of_year(y, got)) { fail("%d 年节气计算失败", y); continue; }
        for (int t = 0; t < 24; t++) {
            g_checks++;
            int wm = VEC_TERM_MD[i * 24 + t][0], wd = VEC_TERM_MD[i * 24 + t][1];
            if (got[t].month != wm || got[t].day != wd)
                fail("%d 年 %s: got %02d-%02d, want %02d-%02d",
                     y, VEC_TERM_NAMES[t], got[t].month, got[t].day, wm, wd);
        }
    }

    /* ---- ③ 逐日全信息 ---- */
    printf("  · ⑤ 逐日全信息 %d 天（原始值 + 去重后有效值）\n", VEC_DAY_COUNT);
    for (int i = 0; i < VEC_DAY_COUNT; i++) {
        const vec_day *v = &VEC_DAYS[i];
        cal_date cd = { v->y, v->m, v->d };

        expect_i("星期", cal_weekday(v->y, v->m, v->d), v->weekday, v->y, v->m, v->d);
        expect_i("星期(Zeller 交叉)", zeller_weekday(v->y, v->m, v->d), v->weekday,
                 v->y, v->m, v->d);

        g_checks++;
        if (!cal_lunar_from_solar(&cd, &lu)) {
            fail("农历换算失败 %04d-%02d-%02d", v->y, v->m, v->d);
            continue;
        }
        expect_i("农历月", lu.month, v->lmonth, v->y, v->m, v->d);
        expect_i("农历日", lu.day, v->lday, v->y, v->m, v->d);
        expect_i("闰月标志", lu.is_leap ? 1 : 0, v->lleap, v->y, v->m, v->d);

        /* 原始层 */
        expect_i("节气索引", cal_solar_term_index(&cd), v->term, v->y, v->m, v->d);
        char fbuf[CAL_FEST_MAX][CAL_FEST_NAME_MAX];
        int fn = cal_festivals(&cd, fbuf);
        expect_i("节日个数", fn, v->fest_n, v->y, v->m, v->d);
        for (int k = 0; k < fn && k < CAL_FEST_MAX; k++) {
            g_checks++;
            const char *want = VEC_FEST_NAMES[v->fest_id[k]];
            if (strcmp(fbuf[k], want) != 0)
                fail("节日[%d] %04d-%02d-%02d: got \"%s\", want \"%s\"",
                     k, v->y, v->m, v->d, fbuf[k], want);
        }

        /* 汇总层（含「清明让位」） */
        cal_day_info info;
        cal_day_info_get(&cd, &info);
        g_checks++;
        const char *want_term = (v->term_eff < 0) ? "" : VEC_TERM_NAMES[v->term_eff];
        if (strcmp(info.term, want_term) != 0)
            fail("有效节气 %04d-%02d-%02d: got \"%s\", want \"%s\"",
                 v->y, v->m, v->d, info.term, want_term);
        expect_i("有效节日个数", info.fest_count, v->fest_eff_n, v->y, v->m, v->d);
        for (int k = 0; k < info.fest_count && k < CAL_FEST_MAX; k++) {
            g_checks++;
            const char *want = VEC_FEST_NAMES[v->fest_eff_id[k]];
            if (strcmp(info.festivals[k], want) != 0)
                fail("有效节日[%d] %04d-%02d-%02d: got \"%s\", want \"%s\"",
                     k, v->y, v->m, v->d, info.festivals[k], want);
        }
        /* weekday_cn / date_cn 格式 */
        expect_i("weekday 字段", info.weekday, v->weekday, v->y, v->m, v->d);
    }

    /* ---- ④ 除夕 ---- */
    printf("  · ⑥ 除夕清单 %d 天（命中 + 前后一天不误报）\n", VEC_EVE_COUNT);
    for (int i = 0; i < VEC_EVE_COUNT; i++) {
        cal_date e = { VEC_EVE[i].y, VEC_EVE[i].m, VEC_EVE[i].d };
        g_checks++;
        if (!cal_is_lunar_new_year_eve(&e))
            fail("漏报除夕 %04d-%02d-%02d", e.year, e.month, e.day);

        /* 次日必是正月初一 → 不可能是除夕 */
        cal_date nx = e;
        day_shift(&nx, 1);
        g_checks++;
        if (cal_is_lunar_new_year_eve(&nx))
            fail("误报除夕 %04d-%02d-%02d（正月初一）", nx.year, nx.month, nx.day);

        /* 前一日也不该是除夕（两个腊月廿九/三十连续出现不可能） */
        cal_date pv = e;
        day_shift(&pv, -1);
        g_checks++;
        if (cal_is_lunar_new_year_eve(&pv))
            fail("误报除夕 %04d-%02d-%02d（除夕前一天）", pv.year, pv.month, pv.day);
    }

    /* ---- ⑤ 撞车 ---- */
    printf("  · ⑦ 多节日撞车 %d 天\n", VEC_COLL_COUNT);
    for (int i = 0; i < VEC_COLL_COUNT; i++) {
        cal_date cd = { VEC_COLL[i].y, VEC_COLL[i].m, VEC_COLL[i].d };
        char fbuf[CAL_FEST_MAX][CAL_FEST_NAME_MAX];
        int fn = cal_festivals(&cd, fbuf);
        expect_i("撞车个数", fn, VEC_COLL[i].n, cd.year, cd.month, cd.day);
        for (int k = 0; k < fn && k < CAL_FEST_MAX; k++) {
            g_checks++;
            const char *want = VEC_FEST_NAMES[VEC_COLL[i].id[k]];
            if (strcmp(fbuf[k], want) != 0)
                fail("撞车[%d] %04d-%02d-%02d: got \"%s\", want \"%s\"",
                     k, cd.year, cd.month, cd.day, fbuf[k], want);
        }
    }

    /* ---- ⑥ 农历串 ---- */
    printf("  · ⑧ 农历中文串 %d 条\n", VEC_TEXT_COUNT);
    for (int i = 0; i < VEC_TEXT_COUNT; i++) {
        cal_date cd = { VEC_TEXT[i].y, VEC_TEXT[i].m, VEC_TEXT[i].d };
        char buf[CAL_TEXT_MAX];
        if (!cal_lunar_from_solar(&cd, &lu)) { fail("农历串换算失败"); continue; }
        cal_lunar_text(&lu, buf, sizeof(buf));
        expect_s("农历串", buf, VEC_TEXT[i].text, cd.year, cd.month, cd.day);
    }

    /* ---- ⑦ 全域自洽 ---- */
    section("⑨ 全域自洽 1900-01-31..2099-12-31 逐日往返");
    {
        cal_date cur = { 1900, 1, 31 };
        long n = 0;
        for (;;) {
            if (!cal_lunar_from_solar(&cur, &lu)) {
                fail("全域：换算失败 %04d-%02d-%02d", cur.year, cur.month, cur.day);
                break;
            }
            cal_date back;
            if (!cal_lunar_to_solar(&lu, &back)) {
                fail("全域：反算失败 %04d-%02d-%02d", cur.year, cur.month, cur.day);
                break;
            }
            g_checks++;
            if (back.year != cur.year || back.month != cur.month || back.day != cur.day) {
                fail("全域：往返不等 %04d-%02d-%02d -> %04d-%02d-%02d",
                     cur.year, cur.month, cur.day, back.year, back.month, back.day);
                break;
            }
            /* 农历日号必须落在该月实际长度内 */
            g_checks++;
            int len = lu.is_leap ? cal_lunar_leap_days(lu.year)
                                 : cal_lunar_month_days(lu.year, lu.month);
            if (len == 0 || lu.day < 1 || lu.day > len) {
                fail("全域：日号超出月长 %04d-%02d-%02d（农历 %d-%d 长 %d）",
                     cur.year, cur.month, cur.day, lu.month, lu.day, len);
                break;
            }
            n++;
            if (cur.year == 2099 && cur.month == 12 && cur.day == 31) break;
            day_shift(&cur, 1);
        }
        printf("     逐日往返 %ld 天\n", n);
    }

    /* ---- ⑧ 边界 ---- */
    section("⑩ 年表边界");
    {
        cal_date a = { 1899, 12, 31 };
        g_checks++;
        if (cal_lunar_from_solar(&a, &lu)) fail("1899-12-31 应被拒绝");
        a.year = 1900; a.month = 1; a.day = 30;
        g_checks++;
        if (cal_lunar_from_solar(&a, &lu)) fail("1900-01-30 应被拒绝（年表起点是 01-31）");
        a.day = 31;
        g_checks++;
        if (!cal_lunar_from_solar(&a, &lu)) fail("1900-01-31 应成功");
        else expect_i("1900-01-31 农历", lu.month * 100 + lu.day, 101, 1900, 1, 31);
        a.year = 2100; a.month = 1; a.day = 1;
        g_checks++;
        if (cal_lunar_from_solar(&a, &lu)) fail("2100-01-01 应被拒绝");
        /* 2099-12-31 必须成功 */
        a.year = 2099; a.month = 12; a.day = 31;
        g_checks++;
        if (!cal_lunar_from_solar(&a, &lu)) fail("2099-12-31 应成功");
    }

    printf("\n");
    printf("  ── %d 项断言，%d 项失败 ──\n", g_checks, g_fails);
    printf("%s\n", g_fails == 0 ? "✅ 全部通过：C 实现与 Python 参照实现一致"
                                : "❌ 存在不一致，见上");
    return g_fails == 0 ? 0 : 1;
}
