/* 排班取数 —— 响应解析、缓存与服务器时间换算（纯 C，零 ESP-IDF 依赖）。
 *
 * 接口真源是 `docs/11-AGC接口调用说明.md`（2026-10-22 实测成功的那一次）：
 * 先换 access_token，再带 Bearer 调中转云函数，响应形如
 *
 *     {"code":200,"message":"查询成功",
 *      "data":{"groupId":"...","startDate":"...","endDate":"...",
 *              "days":[{"date":"2026-10-23","shiftName":"9DI",
 *                       "amStart":"12:30","amEnd":"17:00",
 *                       "pmStart":"18:00","pmEnd":"21:00","remark":""}, ...]}}
 *
 * ⚠️ 服务端用**省略键**表达「这天没排班」（docs/11 §5.5）—— 既不是空字符串，
 * 也不是 null。因此判据是「`shiftName` 键是否存在」，不是值是否为空。
 * 缺字段 ≠ 拉取失败：前者是业务结果，后者才显示「获取失败」（docs/03 §2.2）。
 *
 * 本文件刻意不引入任何 IDF 头：解析器会在 PC 上用宿主编译器再编一遍，
 * 拿真实响应做逐项断言（`tools/sched_parse_test.py`），与 cal_almanac / cal_render
 * 的验证方式一致。设备侧的 HTTP 与 NVS 在 `cal_sched_net.h` 里。
 */
#ifndef CAL_SCHED_H_
#define CAL_SCHED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 拉取窗口天数。docs/03 §6.2 定的是未来 14 天滚动窗口，服务端实测回 14 条。
 * 超过这个数量的条目**直接判违约**而不是丢弃 —— 静默丢弃会让缓存与页面悄悄对不上。 */
#define CAL_SCHED_MAX_DAYS 14
/* 班次名上限。实测最长是 `9DP0（验车）` 16 字节（UTF-8）；留到 40 是为了容下
 * 还没见过的备注词，同时把 NVS 单条 blob 压在 2 KB 以内。 */
#define CAL_SCHED_SHIFT_MAX 40
/* 时段上限。`HH:MM-HH:MM` 是 11 字节，接口实际给的就是这个格式。 */
#define CAL_SCHED_SPAN_MAX 16
#define CAL_SCHED_MESSAGE_MAX 32

/* 一天的排班。shift / am / pm 存**原始串**，归一化（全角括号 → 半角）由渲染器做，
 * 与 PC 侧 `calendar_A_final.py` 的职责划分一致。
 *
 * `has_schedule == false` 表示服务端省了 `shiftName` 键 → 这天没有排班数据。
 * 渲染层据此置 blank_schedule，主区留白、底栏不画班次块。 */
typedef struct {
    int year;
    int month;
    int day;
    bool has_schedule;
    char shift[CAL_SCHED_SHIFT_MAX];
    char am[CAL_SCHED_SPAN_MAX];
    char pm[CAL_SCHED_SPAN_MAX];
} CalSchedDay;

typedef struct {
    int code;                       /* 业务码，200 才算查询成功 */
    char message[CAL_SCHED_MESSAGE_MAX];
    int count;
    CalSchedDay days[CAL_SCHED_MAX_DAYS];
} CalSchedPayload;

/* 解析云函数响应体。
 *
 * 返回 true 的前提：合法 JSON、`code == 200`、`data.days` 是数组、条数在
 * 1..CAL_SCHED_MAX_DAYS 之间、每条 `date` 都是 `YYYY-MM-DD`、字段长度不越界。
 * 任何一项不满足都返回 false 并把原因写进日志 —— 数据源违约要暴露出来，
 * 不能靠截断或跳过把它抹平（截断会让页面显示错误的班次）。
 * json 不需要以 NUL 结尾。 */
bool cal_sched_parse(const char* json, size_t len, CalSchedPayload* out);

/* 最近一次解析失败的原因（静态缓冲，仅用于日志）。成功解析后为空串。 */
const char* cal_sched_last_error(void);

/* 从 `YYYY-MM-DD` 取年月日。严格 10 字符、分隔符必须是 '-'、月日范围合法。 */
bool cal_sched_parse_date(const char* text, int* year, int* month, int* day);

/* ---------------------------------------------------------------- 缓存 */
/*
 * 缓存是断网兜底（docs/03 §2.4）：当天数据大概率已经在前一次 14 天窗口里。
 * 格式显式按字节序列化，不直接 memcpy 结构体 —— 结构体填充字节的内容不确定，
 * 而且 PC 侧测试要能跨编译器读同一份字节。
 */
#define CAL_SCHED_CACHE_MAGIC 0x5A435343u /* "ZCSS" */
#define CAL_SCHED_CACHE_VERSION 1
/* 头部：magic(4) + version(1) + count(1) + 保留(2) + fetched_epoch(8) = 16 字节。
 * 每条：year(2) + month(1) + day(1) + flags(1) + 保留(1) + shift(40) + am(16) + pm(16) = 78 字节。
 * 14 条 + 头部 = 1108 字节，NVS 单条 blob 上限 ~4000 字节，装得下。 */
#define CAL_SCHED_CACHE_HEADER_BYTES 16
#define CAL_SCHED_CACHE_ENTRY_BYTES 78
#define CAL_SCHED_CACHE_BYTES                                              \
    (CAL_SCHED_CACHE_HEADER_BYTES + CAL_SCHED_MAX_DAYS * CAL_SCHED_CACHE_ENTRY_BYTES)

typedef struct {
    int64_t fetched_epoch; /* 写入缓存的时刻（UTC 秒），0 表示从未成功拉取 */
    int count;
    CalSchedDay days[CAL_SCHED_MAX_DAYS];
} CalSchedCache;

void cal_sched_cache_clear(CalSchedCache* cache);

/* 序列化到 out。out_size 不足返回 0（不写半截数据）。成功返回写入字节数。 */
size_t cal_sched_cache_pack(const CalSchedCache* in, uint8_t* out, size_t out_size);

/* 反序列化。magic / version / count / 长度任一不符返回 false，out 保持清空状态。 */
bool cal_sched_cache_unpack(const uint8_t* in, size_t len, CalSchedCache* out);

/* 按日期查缓存。找到返回 true 并填 out；缓存里没有这一天返回 false。
 * 命中但 `has_schedule == false` 是正常结果（那天确实没排班）。 */
bool cal_sched_cache_find(const CalSchedCache* cache, int year, int month, int day,
                          CalSchedDay* out);

/* ---------------------------------------------------------------- 服务器时间 */
/*
 * docs/03 §6.1 定的校时来源：HTTPS 响应头的 `Date`，零额外请求。
 * 实测响应头形如 `Thu, 22 Oct 2026 09:40:56 GMT`（RFC 1123，GMT = UTC）。
 */
bool cal_sched_parse_http_date(const char* text, int64_t* epoch_seconds);

/* UTC 秒 → 北京时间（固定 UTC+8，无夏令时）。
 * 自己算而不是用 localtime_r：板上 TZ 环境变量与 PC 侧测试环境不一定一致，
 * 校时算错会把整页日期带偏，这一环要确定。 */
bool cal_sched_beijing_time(int64_t epoch_seconds, int* year, int* month, int* day,
                            int* hour, int* minute, int* second);

/* 由年月日算 UTC 秒（当天 00:00:00 UTC）。给自检与测试用。 */
int64_t cal_sched_epoch_from_date(int year, int month, int day);

/* ---------------------------------------------------------------- 令牌有效期 */
/*
 * 访问令牌是否能直接复用。判据 = 过期时刻减去安全余量之后仍晚于当前时间。
 *
 * 留余量是为了吸收两类边界：本地时钟刚被校准/尚未校准，以及「请求在路上花掉
 * 几秒」—— 拿一个只剩 30 秒的令牌去调用，响应回来时它可能已经失效，白跑一轮。
 * 时间戳是本地时间（北京时间）算出来的，两边口径必须一致。
 */
bool cal_sched_token_usable(int64_t expire_epoch, int64_t now_epoch, int32_t margin_seconds);

#ifdef __cplusplus
}
#endif

#endif /* CAL_SCHED_H_ */
