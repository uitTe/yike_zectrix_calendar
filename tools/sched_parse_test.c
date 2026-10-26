/* 排班解析器 / 缓存的宿主侧测试 —— 用合成样例逐项断言。
 *
 * 样例（`tools/testdata/agc_response_20261022.json`）的结构取自 2026-10-22 的实测响应，
 * 班次名与分组编号已替换为虚构值：开源仓库不放真实排班数据。三种形态都保留 ——
 * 休息日、带全角括号的备注、窗口末尾省略 shiftName。契约见 `docs/11` §5.4。
 *
 * 为什么要在 PC 上再测一遍：解析器是整条取数链路里唯一「服务端格式一改就静默出错」
 * 的地方 —— 字段缺省被判成拉取失败，页面就会把正常的未排班日画成「获取失败」，
 * 而设备一天只醒一次，错了当天没有第二次机会。
 *
 * 编译运行由 `tools/sched_parse_test.py` 负责（需要 cJSON 源码 + 宿主 gcc）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cal_sched.h"

static int g_bad = 0;
static int g_checks = 0;

#define CHECK_STR(actual, expected, what)                                          \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (strcmp((actual), (expected)) != 0) {                                     \
            printf("FAIL %s: 实际 \"%s\"，期望 \"%s\"\n", what, actual, expected);   \
            ++g_bad;                                                                \
        }                                                                           \
    } while (0)

#define CHECK_INT(actual, expected, what)                                          \
    do {                                                                            \
        ++g_checks;                                                                 \
        const long a_ = (long)(actual);                                             \
        const long e_ = (long)(expected);                                           \
        if (a_ != e_) {                                                             \
            printf("FAIL %s: 实际 %ld，期望 %ld\n", what, a_, e_);                   \
            ++g_bad;                                                                \
        }                                                                           \
    } while (0)

#define CHECK_TRUE(cond, what)                                                     \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            printf("FAIL %s\n", what);                                              \
            ++g_bad;                                                                \
        }                                                                           \
    } while (0)

static char* ReadFile(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        printf("FAIL 打不开样本文件 %s\n", path);
        exit(2);
    }
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* buffer = (char*)malloc((size_t)length + 1);
    if (buffer == NULL || fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        printf("FAIL 读取样本文件失败 %s\n", path);
        exit(2);
    }
    buffer[length] = '\0';
    *size = (size_t)length;
    fclose(file);
    return buffer;
}

/* 真实响应：14 天窗口，其中 11-01 至 11-04 只有 date 与空 remark。 */
static void TestRealResponse(const char* json, size_t size) {
    CalSchedPayload payload;
    CHECK_TRUE(cal_sched_parse(json, size, &payload), "真实响应应解析成功");
    if (g_bad != 0) {
        printf("      解析失败原因：%s\n", cal_sched_last_error());
        return;
    }
    CHECK_INT(payload.code, 200, "业务码");
    CHECK_STR(payload.message, "查询成功", "业务消息");
    CHECK_INT(payload.count, 14, "天数");

    /* 休班：班次名保留（底栏反白块要显示「休」），时段按 docs/03 §2.1 置空。 */
    const CalSchedDay* d = &payload.days[0];
    CHECK_INT(d->year, 2026, "第 1 天 year");
    CHECK_INT(d->month, 10, "第 1 天 month");
    CHECK_INT(d->day, 22, "第 1 天 day");
    CHECK_INT(d->has_schedule, 1, "第 1 天有排班");
    CHECK_STR(d->shift, "休", "第 1 天班次");
    CHECK_STR(d->am, "", "第 1 天第 1 段（休班应忽略）");
    CHECK_STR(d->pm, "", "第 1 天第 2 段（休班应忽略）");

    d = &payload.days[1];
    CHECK_STR(d->shift, "A1", "第 2 天班次");
    CHECK_STR(d->am, "12:30-17:00", "第 2 天第 1 段");
    CHECK_STR(d->pm, "18:00-21:00", "第 2 天第 2 段");

    /* 全角括号必须原样保留 —— 归一化是渲染器的职责（cal_shift_normalize）。 */
    d = &payload.days[2];
    CHECK_STR(d->shift, "C2（验车）", "第 3 天班次（全角括号保留原样）");
    CHECK_STR(d->am, "09:00-12:00", "第 3 天第 1 段");

    d = &payload.days[4];
    CHECK_STR(d->shift, "B1", "10-26 班次");
    CHECK_STR(d->am, "09:30-13:15", "10-26 第 1 段");
    CHECK_STR(d->pm, "17:00-21:00", "10-26 第 2 段");

    /* 11-01 起服务端省略 shiftName —— 是「没排班」，不是「拉取失败」。 */
    for (int i = 10; i < 14; ++i) {
        d = &payload.days[i];
        CHECK_INT(d->has_schedule, 0, "11-01 之后的条目应无排班");
        CHECK_STR(d->shift, "", "无排班条目班次应为空");
        CHECK_STR(d->am, "", "无排班条目第 1 段应为空");
    }
    CHECK_INT(payload.days[13].day, 4, "最后一天 day");
}

/* 缓存往返：格式错会让页面空白，而这类错误在设备上没有任何提示。 */
static void TestCacheRoundTrip(const CalSchedPayload* payload) {
    CalSchedCache cache;
    CalSchedCache restored;
    uint8_t buffer[CAL_SCHED_CACHE_BYTES];
    cal_sched_cache_clear(&cache);
    cache.fetched_epoch = 1792650056;
    cache.count = payload->count;
    for (int i = 0; i < payload->count; ++i) {
        cache.days[i] = payload->days[i];
    }

    const size_t written = cal_sched_cache_pack(&cache, buffer, sizeof(buffer));
    CHECK_INT(written, CAL_SCHED_CACHE_BYTES, "缓存序列化字节数");
    CHECK_TRUE(cal_sched_cache_unpack(buffer, written, &restored), "缓存应能反序列化");
    CHECK_INT(restored.fetched_epoch, cache.fetched_epoch, "缓存拉取时刻");
    CHECK_INT(restored.count, cache.count, "缓存条数");
    for (int i = 0; i < cache.count; ++i) {
        const CalSchedDay* a = &cache.days[i];
        const CalSchedDay* b = &restored.days[i];
        CHECK_INT(b->year, a->year, "缓存 year");
        CHECK_INT(b->month, a->month, "缓存 month");
        CHECK_INT(b->day, a->day, "缓存 day");
        CHECK_INT(b->has_schedule, a->has_schedule, "缓存 has_schedule");
        CHECK_STR(b->shift, a->shift, "缓存 shift");
        CHECK_STR(b->am, a->am, "缓存 am");
        CHECK_STR(b->pm, a->pm, "缓存 pm");
    }

    /* 缓冲不足时必须拒绝写，不能写出半截数据。 */
    CHECK_INT(cal_sched_cache_pack(&cache, buffer, CAL_SCHED_CACHE_BYTES - 1), 0,
              "缓冲不足应拒绝序列化");

    CalSchedDay found;
    CHECK_TRUE(cal_sched_cache_find(&restored, 2026, 10, 26, &found), "应能查到 10-26");
    CHECK_STR(found.shift, "B1", "查到 10-26 的班次");
    CHECK_TRUE(cal_sched_cache_find(&restored, 2026, 11, 2, &found), "应能查到 11-02");
    CHECK_INT(found.has_schedule, 0, "11-02 无排班");
    CHECK_TRUE(!cal_sched_cache_find(&restored, 2026, 12, 1, &found),
               "窗口外的日期不应命中");

    /* 长度截断必须被识别。反序列化失败会把 out 清空，所以用独立变量，
     * 免得把上面已经验过的 restored 一起清掉。 */
    CalSchedCache rejected;
    CHECK_TRUE(!cal_sched_cache_unpack(buffer, 20, &rejected), "截断的缓存应被拒绝");
    CHECK_INT(rejected.count, 0, "被拒绝的缓存应保持清空");
}

/* 服务端违约样本：每一种都必须被判失败，不能靠截断或跳过蒙过去。 */
static void TestViolations(void) {
    CalSchedPayload payload;

    const char* bad_code =
        "{\"code\":500,\"message\":\"查询失败\",\"data\":{\"days\":[]}}";
    CHECK_TRUE(!cal_sched_parse(bad_code, strlen(bad_code), &payload),
               "业务码 500 应判失败");
    CHECK_STR(payload.message, "查询失败", "失败时仍应带出服务端消息");

    const char* missing_date =
        "{\"code\":200,\"message\":\"查询成功\",\"data\":{\"days\":"
        "[{\"shiftName\":\"9DP1\"}]}}";
    CHECK_TRUE(!cal_sched_parse(missing_date, strlen(missing_date), &payload),
               "缺 date 应判失败");

    const char* bad_date =
        "{\"code\":200,\"message\":\"查询成功\",\"data\":{\"days\":"
        "[{\"date\":\"2026-13-01\"}]}}";
    CHECK_TRUE(!cal_sched_parse(bad_date, strlen(bad_date), &payload),
               "非法月份应判失败");

    const char* not_json = "<html>502 Bad Gateway</html>";
    CHECK_TRUE(!cal_sched_parse(not_json, strlen(not_json), &payload),
               "非 JSON 应判失败");

    const char* no_days = "{\"code\":200,\"message\":\"查询成功\",\"data\":{}}";
    CHECK_TRUE(!cal_sched_parse(no_days, strlen(no_days), &payload),
               "缺 days 数组应判失败");

    /* 15 条超出 14 天窗口 —— 判违约而不是丢弃第 15 条。 */
    char many[512];
    size_t used = sprintf(many, "{\"code\":200,\"data\":{\"days\":[");
    for (int i = 0; i < 15; ++i) {
        used += sprintf(many + used, "%s{\"date\":\"2026-10-%02d\"}",
                        i == 0 ? "" : ",", i + 1);
    }
    sprintf(many + used, "]}}");
    CHECK_TRUE(!cal_sched_parse(many, strlen(many), &payload), "15 条应判失败");

    /* 空字符串班次名等同没有排班（服务端两种写法都要认）。 */
    const char* empty_shift =
        "{\"code\":200,\"data\":{\"days\":[{\"date\":\"2026-10-22\","
        "\"shiftName\":\"\",\"amStart\":\"08:00\",\"amEnd\":\"12:00\"}]}}";
    CHECK_TRUE(cal_sched_parse(empty_shift, strlen(empty_shift), &payload),
               "空班次名应解析成功");
    CHECK_INT(payload.count, 1, "空班次名条数");
    CHECK_INT(payload.days[0].has_schedule, 0, "空班次名视为无排班");

    /* 只给半段的时段：置空，不拼出 "09:00-" 这种半截串。 */
    const char* half_span =
        "{\"code\":200,\"data\":{\"days\":[{\"date\":\"2026-10-22\","
        "\"shiftName\":\"9DP1\",\"amStart\":\"09:00\"}]}}";
    CHECK_TRUE(cal_sched_parse(half_span, strlen(half_span), &payload),
               "半段时段应解析成功");
    CHECK_STR(payload.days[0].am, "", "半段时段应留空");
}

/* docs/03 §6.1：时间校准来源是 HTTPS 响应头的 Date。实测值见 docs/11 §4.3。 */
static void TestServerTime(void) {
    int64_t epoch = 0;
    CHECK_TRUE(cal_sched_parse_http_date("Thu, 22 Oct 2026 09:40:56 GMT", &epoch),
               "Date 头应能解析");
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    CHECK_TRUE(cal_sched_beijing_time(epoch, &y, &mo, &d, &h, &mi, &s), "应能换算北京时间");
    CHECK_INT(y, 2026, "Date 头年");
    CHECK_INT(mo, 10, "Date 头月");
    CHECK_INT(d, 22, "Date 头日");
    CHECK_INT(h, 17, "Date 头时（UTC+8）");
    CHECK_INT(mi, 40, "Date 头分");
    CHECK_INT(s, 56, "Date 头秒");

    /* 跨日边界：UTC 16:00 正好是北京时间次日 00:00 —— 校时算错一天，
     * 整页日期、农历、节气、节日全部错位。 */
    const int64_t midnight = cal_sched_epoch_from_date(2026, 10, 22) + 16 * 3600;
    CHECK_TRUE(cal_sched_beijing_time(midnight, &y, &mo, &d, &h, &mi, &s),
               "跨日边界应能换算");
    CHECK_INT(d, 23, "跨日边界应落在 23 日");
    CHECK_INT(h, 0, "跨日边界应是 0 点");

    CHECK_TRUE(cal_sched_parse_http_date("Sun, 01 Mar 2026 00:00:00 GMT", &epoch),
               "3 月 1 日应能解析");
    CHECK_TRUE(cal_sched_beijing_time(epoch, &y, &mo, &d, &h, &mi, &s), "3 月 1 日换算");
    CHECK_INT(mo, 3, "3 月月份");
    CHECK_INT(d, 1, "3 月日期");
    CHECK_INT(h, 8, "3 月 1 日北京时间 8 点");

    CHECK_TRUE(!cal_sched_parse_http_date("", &epoch), "空 Date 头应判失败");
    CHECK_TRUE(!cal_sched_parse_http_date("Wed, 32 Xyz 2026 09:40:56 GMT", &epoch),
               "非法月份应判失败");

    /* 2027-02-06 是公历闰年判断的边界样本（2028 才是闰年）。 */
    CHECK_INT(cal_sched_epoch_from_date(1970, 1, 1), 0, "1970-01-01 应为 0");
    CHECK_INT(cal_sched_epoch_from_date(2000, 3, 1) -
                  cal_sched_epoch_from_date(2000, 2, 1),
              29 * 86400, "2000 年 2 月有 29 天");
    CHECK_INT(cal_sched_epoch_from_date(2100, 3, 1) -
                  cal_sched_epoch_from_date(2100, 2, 1),
              28 * 86400, "2100 年 2 月有 28 天");
}

/* 令牌有效期判定：设备一天醒一次、令牌活两天，复用判据错一次就会每天多换一次。 */
static void TestTokenUsable(void) {
    const int64_t now = 1792650056; /* 2026-10-22 15:40:56 UTC 附近的任意时刻 */
    const int32_t margin = 3600;

    CHECK_TRUE(cal_sched_token_usable(now + 7200, now, margin), "还剩 2 小时应可用");
    CHECK_TRUE(!cal_sched_token_usable(now + 1800, now, margin),
               "只剩 30 分钟应判过期（余量吃掉）");
    CHECK_TRUE(!cal_sched_token_usable(now + margin, now, margin),
               "刚好等于余量应判过期（判据是严格小于）");
    CHECK_TRUE(!cal_sched_token_usable(now - 10, now, margin), "已过期应不可用");
    CHECK_TRUE(!cal_sched_token_usable(0, now, margin), "没有过期时刻应不可用");
    /* 设备时钟被重置到 1970 时，本地判定一定说「还没过期」—— 这时靠云函数 401
     * 的兜底去重换令牌，不能在这里判死。 */
    CHECK_TRUE(cal_sched_token_usable(now + 7200, 0, margin), "时钟回退时按可用处理");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法：%s <真实响应样本.json>\n", argv[0]);
        return 2;
    }
    size_t size = 0;
    char* json = ReadFile(argv[1], &size);

    TestRealResponse(json, size);
    if (g_bad == 0) {
        CalSchedPayload payload;
        cal_sched_parse(json, size, &payload);
        TestCacheRoundTrip(&payload);
    }
    TestViolations();
    TestServerTime();
    TestTokenUsable();
    free(json);

    printf("sched parse test: %d checks, %d mismatch\n", g_checks, g_bad);
    return g_bad == 0 ? 0 : 1;
}
