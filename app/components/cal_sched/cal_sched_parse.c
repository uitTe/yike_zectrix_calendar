// 排班响应解析、缓存序列化、服务器时间换算。纯 C99，零 ESP-IDF 依赖 ——
// 同一份源码会在 PC 上再编一遍做逐项断言（tools/sched_parse_test.py）。
#include "cal_sched.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define ERROR_MAX 160

static char g_last_error[ERROR_MAX] = "";

static void SetError(const char* text, const char* detail) {
    if (detail != NULL) {
        snprintf(g_last_error, sizeof(g_last_error), "%s: %s", text, detail);
    } else {
        snprintf(g_last_error, sizeof(g_last_error), "%s", text);
    }
}

/* UTF-8「休」= E4 BC 91。班次名里出现它就算休息日（实测形态是单个「休」，
 * 放宽为包含以兼容「休息」）。判据与 cal_render.cc 的 cal_shift_is_rest 一致。 */
static int ContainsRest(const char* text) {
    if (text == NULL) {
        return 0;
    }
    const size_t len = strlen(text);
    for (size_t i = 0; i + 2 < len; ++i) {
        if ((unsigned char)text[i] == 0xE4 && (unsigned char)text[i + 1] == 0xBC &&
            (unsigned char)text[i + 2] == 0x91) {
            return 1;
        }
    }
    return 0;
}

/* 从 cJSON 对象取字符串字段。键不存在返回 NULL（服务端省略键就是这种情况）；
 * 键存在但不是字符串则返回 NULL 且 *bad 置位 —— 后者属于格式违约。 */
static const char* StringField(const cJSON* obj, const char* key, int* bad) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == NULL) {
        return NULL;
    }
    if (!cJSON_IsString(item)) {
        *bad = 1;
        return NULL;
    }
    return item->valuestring;
}

/* 拼 `开始-结束`。长度按 开始 + 1 + 结束 + 末尾 NUL 精确校验后再拷，
 * 不用 snprintf 拼接 —— 后者在超长时会静默截断，这里必须硬失败。 */
static int JoinSpan(const char* begin, const char* end, char* out, size_t out_size) {
    const size_t begin_len = strlen(begin);
    const size_t end_len = strlen(end);
    if (begin_len + end_len + 2 > out_size) {
        return 0;
    }
    memcpy(out, begin, begin_len);
    out[begin_len] = '-';
    memcpy(out + begin_len + 1, end, end_len + 1);
    return 1;
}

/* ---------------------------------------------------------------- 日期换算 */
/* Howard Hinnant 的 days_from_civil / civil_from_days：公历与「1970-01-01 起的天数」
 * 互转，不依赖 timegm / gmtime —— 这两个在 newlib 与 MSVC 上的可用性不一致。 */
static int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    const int y = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (month + (month > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void CivilFromDays(int64_t z, int* year, unsigned* month, unsigned* day) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    const unsigned m = mp + (mp < 10u ? 3u : -9u);
    *year = (int)(y + (m <= 2 ? 1 : 0));
    *month = m;
    *day = d;
}

static int MonthFromName(const char* name) {
    static const char* kNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i) {
        if (strncmp(name, kNames[i], 3) == 0) {
            return i + 1;
        }
    }
    return 0;
}

const char* cal_sched_last_error(void) { return g_last_error; }

bool cal_sched_parse_date(const char* text, int* year, int* month, int* day) {
    static const int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (text == NULL || strlen(text) != 10 || text[4] != '-' || text[7] != '-') {
        return 0;
    }
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
    }
    const int y = (text[0] - '0') * 1000 + (text[1] - '0') * 100 + (text[2] - '0') * 10 +
                  (text[3] - '0');
    const int m = (text[5] - '0') * 10 + (text[6] - '0');
    const int d = (text[8] - '0') * 10 + (text[9] - '0');
    if (y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) {
        return 0;
    }
    int limit = kMonthDays[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
        limit = 29;
    }
    if (d > limit) {
        return 0;
    }
    *year = y;
    *month = m;
    *day = d;
    return 1;
}

bool cal_sched_parse(const char* json, size_t len, CalSchedPayload* out) {
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    g_last_error[0] = '\0';
    if (json == NULL || len == 0) {
        SetError("响应体为空", NULL);
        return 0;
    }

    cJSON* root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        SetError("响应体不是合法 JSON", NULL);
        return 0;
    }
    if (!cJSON_IsObject(root)) {
        SetError("响应体顶层不是对象", NULL);
        cJSON_Delete(root);
        return 0;
    }

    /* 业务码与消息先填进去：即使失败，日志也要能打出服务端到底说了什么。 */
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code)) {
        SetError("响应体缺少数字字段 code", NULL);
        cJSON_Delete(root);
        return 0;
    }
    out->code = code->valueint;
    const cJSON* message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsString(message)) {
        snprintf(out->message, sizeof(out->message), "%s", message->valuestring);
    }

    if (out->code != 200) {
        SetError("业务码不是 200", out->message[0] != '\0' ? out->message : NULL);
        cJSON_Delete(root);
        return 0;
    }

    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        SetError("响应体缺少对象 data", NULL);
        cJSON_Delete(root);
        return 0;
    }
    const cJSON* days = cJSON_GetObjectItemCaseSensitive(data, "days");
    if (!cJSON_IsArray(days)) {
        SetError("data.days 不是数组", NULL);
        cJSON_Delete(root);
        return 0;
    }
    const int total = cJSON_GetArraySize(days);
    if (total <= 0 || total > CAL_SCHED_MAX_DAYS) {
        char detail[32];
        snprintf(detail, sizeof(detail), "%d 条（允许 1..%d）", total, CAL_SCHED_MAX_DAYS);
        SetError("data.days 条数不符", detail);
        cJSON_Delete(root);
        return 0;
    }

    int index = 0;
    for (const cJSON* item = days->child; item != NULL; item = item->next, ++index) {
        CalSchedDay* day = &out->days[index];
        char detail[40];
        if (!cJSON_IsObject(item)) {
            snprintf(detail, sizeof(detail), "第 %d 条", index + 1);
            SetError("data.days 元素不是对象", detail);
            cJSON_Delete(root);
            return 0;
        }
        const cJSON* date = cJSON_GetObjectItemCaseSensitive(item, "date");
        if (!cJSON_IsString(date)) {
            snprintf(detail, sizeof(detail), "第 %d 条", index + 1);
            SetError("条目缺少字符串字段 date", detail);
            cJSON_Delete(root);
            return 0;
        }
        if (!cal_sched_parse_date(date->valuestring, &day->year, &day->month, &day->day)) {
            SetError("date 不是合法的 YYYY-MM-DD", date->valuestring);
            cJSON_Delete(root);
            return 0;
        }

        int bad = 0;
        const char* shift = StringField(item, "shiftName", &bad);
        const char* am_start = StringField(item, "amStart", &bad);
        const char* am_end = StringField(item, "amEnd", &bad);
        const char* pm_start = StringField(item, "pmStart", &bad);
        const char* pm_end = StringField(item, "pmEnd", &bad);
        if (bad) {
            SetError("排班字段类型不是字符串", date->valuestring);
            cJSON_Delete(root);
            return 0;
        }

        /* 键缺失或空串 → 这天没有排班数据。服务端用省略键表达，见 docs/11 §5.5。 */
        if (shift != NULL && shift[0] != '\0') {
            if (strlen(shift) >= CAL_SCHED_SHIFT_MAX) {
                SetError("班次名超出缓存长度", shift);
                cJSON_Delete(root);
                return 0;
            }
            day->has_schedule = 1;
            snprintf(day->shift, sizeof(day->shift), "%s", shift);
        }

        /* 时段两段拼接。只有起止都在才拼 —— 只给一半属于违约，宁可留空让它显成
         * 缺时段，也不拼出 "09:00-" 这种半截串让渲染器去猜。 */
        if (am_start != NULL && am_end != NULL && am_start[0] != '\0' &&
            am_end[0] != '\0') {
            if (!JoinSpan(am_start, am_end, day->am, sizeof(day->am))) {
                SetError("第 1 段时间段超出缓存长度", am_start);
                cJSON_Delete(root);
                return 0;
            }
        }
        if (pm_start != NULL && pm_end != NULL && pm_start[0] != '\0' &&
            pm_end[0] != '\0') {
            if (!JoinSpan(pm_start, pm_end, day->pm, sizeof(day->pm))) {
                SetError("第 2 段时间段超出缓存长度", pm_start);
                cJSON_Delete(root);
                return 0;
            }
        }

        /* 休息日忽略时段（docs/03 §2.1）：接口对休班给的是 00:00-00:00，照抄会画出
         * 「凌晨00:00-00:00」。班次名保留 —— 底栏的反白块要显示「休」。 */
        if (day->has_schedule && ContainsRest(day->shift)) {
            day->am[0] = '\0';
            day->pm[0] = '\0';
        }
    }
    out->count = index;
    cJSON_Delete(root);
    return 1;
}

/* ---------------------------------------------------------------- 缓存 */

void cal_sched_cache_clear(CalSchedCache* cache) {
    if (cache != NULL) {
        memset(cache, 0, sizeof(*cache));
    }
}

static void PutU16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xFF);
    out[1] = (uint8_t)((value >> 8) & 0xFF);
}

static uint16_t GetU16(const uint8_t* in) {
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

static void PutI64(uint8_t* out, int64_t value) {
    const uint64_t raw = (uint64_t)value;
    for (int i = 0; i < 8; ++i) {
        out[i] = (uint8_t)((raw >> (8 * i)) & 0xFF);
    }
}

static int64_t GetI64(const uint8_t* in) {
    uint64_t raw = 0;
    for (int i = 0; i < 8; ++i) {
        raw |= (uint64_t)in[i] << (8 * i);
    }
    return (int64_t)raw;
}

size_t cal_sched_cache_pack(const CalSchedCache* in, uint8_t* out, size_t out_size) {
    if (in == NULL || out == NULL || out_size < CAL_SCHED_CACHE_BYTES) {
        return 0;
    }
    if (in->count < 0 || in->count > CAL_SCHED_MAX_DAYS) {
        return 0;
    }
    memset(out, 0, CAL_SCHED_CACHE_BYTES);
    PutU16(out + 0, (uint16_t)(CAL_SCHED_CACHE_MAGIC & 0xFFFF));
    PutU16(out + 2, (uint16_t)(CAL_SCHED_CACHE_MAGIC >> 16));
    out[4] = CAL_SCHED_CACHE_VERSION;
    out[5] = (uint8_t)in->count;
    PutI64(out + 8, in->fetched_epoch);

    for (int i = 0; i < in->count; ++i) {
        const CalSchedDay* day = &in->days[i];
        uint8_t* entry = out + CAL_SCHED_CACHE_HEADER_BYTES +
                         (size_t)i * CAL_SCHED_CACHE_ENTRY_BYTES;
        PutU16(entry + 0, (uint16_t)day->year);
        entry[2] = (uint8_t)day->month;
        entry[3] = (uint8_t)day->day;
        entry[4] = day->has_schedule ? 1 : 0;
        /* entry[5] 是保留字节，memset 已清零。 */
        memcpy(entry + 6, day->shift, CAL_SCHED_SHIFT_MAX);
        memcpy(entry + 6 + CAL_SCHED_SHIFT_MAX, day->am, CAL_SCHED_SPAN_MAX);
        memcpy(entry + 6 + CAL_SCHED_SHIFT_MAX + CAL_SCHED_SPAN_MAX, day->pm,
               CAL_SCHED_SPAN_MAX);
    }
    return CAL_SCHED_CACHE_BYTES;
}

bool cal_sched_cache_unpack(const uint8_t* in, size_t len, CalSchedCache* out) {
    if (out == NULL) {
        return 0;
    }
    cal_sched_cache_clear(out);
    if (in == NULL || len < CAL_SCHED_CACHE_HEADER_BYTES) {
        return 0;
    }
    const uint32_t magic =
        (uint32_t)GetU16(in + 0) | ((uint32_t)GetU16(in + 2) << 16);
    if (magic != CAL_SCHED_CACHE_MAGIC || in[4] != CAL_SCHED_CACHE_VERSION) {
        return 0;
    }
    const int count = in[5];
    if (count < 0 || count > CAL_SCHED_MAX_DAYS) {
        return 0;
    }
    if (len < CAL_SCHED_CACHE_HEADER_BYTES +
                  (size_t)count * CAL_SCHED_CACHE_ENTRY_BYTES) {
        return 0;
    }
    out->fetched_epoch = GetI64(in + 8);
    out->count = count;
    for (int i = 0; i < count; ++i) {
        const uint8_t* entry = in + CAL_SCHED_CACHE_HEADER_BYTES +
                               (size_t)i * CAL_SCHED_CACHE_ENTRY_BYTES;
        CalSchedDay* day = &out->days[i];
        day->year = GetU16(entry + 0);
        day->month = entry[2];
        day->day = entry[3];
        day->has_schedule = entry[4] != 0;
        /* 落盘串可能被写坏（Flash 位翻转、旧版本残留），拷贝后强制补 NUL。 */
        memcpy(day->shift, entry + 6, CAL_SCHED_SHIFT_MAX);
        day->shift[CAL_SCHED_SHIFT_MAX - 1] = '\0';
        memcpy(day->am, entry + 6 + CAL_SCHED_SHIFT_MAX, CAL_SCHED_SPAN_MAX);
        day->am[CAL_SCHED_SPAN_MAX - 1] = '\0';
        memcpy(day->pm, entry + 6 + CAL_SCHED_SHIFT_MAX + CAL_SCHED_SPAN_MAX,
               CAL_SCHED_SPAN_MAX);
        day->pm[CAL_SCHED_SPAN_MAX - 1] = '\0';
    }
    return 1;
}

bool cal_sched_cache_find(const CalSchedCache* cache, int year, int month, int day,
                          CalSchedDay* out) {
    if (cache == NULL || out == NULL) {
        return 0;
    }
    /* count 由 unpack / clear 维护，但跳过取数的路径上 cache 可能是值初始化的空对象
     * 之外的形态。越界读会直接 panic，这里按上界挡一道。 */
    if (cache->count < 0 || cache->count > CAL_SCHED_MAX_DAYS) {
        return 0;
    }
    for (int i = 0; i < cache->count; ++i) {
        const CalSchedDay* item = &cache->days[i];
        if (item->year == year && item->month == month && item->day == day) {
            *out = *item;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- 服务器时间 */

int64_t cal_sched_epoch_from_date(int year, int month, int day) {
    return DaysFromCivil(year, (unsigned)month, (unsigned)day) * 86400;
}

bool cal_sched_token_usable(int64_t expire_epoch, int64_t now_epoch, int32_t margin_seconds) {
    if (expire_epoch <= 0) {
        return false;
    }
    return now_epoch + (int64_t)margin_seconds < expire_epoch;
}

bool cal_sched_parse_http_date(const char* text, int64_t* epoch_seconds) {
    char weekday[4] = {0};
    char month_name[4] = {0};
    char zone[4] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int month = 0;

    if (text == NULL || epoch_seconds == NULL) {
        return 0;
    }
    /* 实测头是 `Thu, 22 Oct 2026 09:40:56 GMT`。星期名不参与计算，只用来区分
     * 「带星期」与「不带星期」两种写法（RFC 允许两者）。 */
    int fields = sscanf(text, "%3s, %d %3s %d %d:%d:%d %3s", weekday, &day, month_name,
                        &year, &hour, &minute, &second, zone);
    if (fields != 8) {
        fields = sscanf(text, "%d %3s %d %d:%d:%d %3s", &day, month_name, &year, &hour,
                        &minute, &second, zone);
        if (fields != 7) {
            return 0;
        }
    }
    month = MonthFromName(month_name);
    if (month == 0) {
        return 0;
    }
    if ((zone[0] != 'G' && zone[0] != 'U') || year < 2020 || year > 2099 || day < 1 ||
        day > 31 || hour > 23 || minute > 59 || second > 59) {
        return 0;
    }
    *epoch_seconds =
        DaysFromCivil(year, (unsigned)month, (unsigned)day) * 86400 + hour * 3600 +
        minute * 60 + second;
    return 1;
}

bool cal_sched_beijing_time(int64_t epoch_seconds, int* year, int* month, int* day,
                            int* hour, int* minute, int* second) {
    int64_t local = 0;
    int64_t days = 0;
    int64_t rest = 0;
    unsigned m = 0;
    unsigned d = 0;

    if (year == NULL || month == NULL || day == NULL || hour == NULL || minute == NULL ||
        second == NULL) {
        return 0;
    }
    /* 北京时间固定 UTC+8，没有夏令时。 */
    local = epoch_seconds + 8 * 3600;
    days = local / 86400;
    rest = local % 86400;
    if (rest < 0) {
        rest += 86400;
        days -= 1;
    }
    CivilFromDays(days, year, &m, &d);
    *month = (int)m;
    *day = (int)d;
    *hour = (int)(rest / 3600);
    *minute = (int)((rest % 3600) / 60);
    *second = (int)(rest % 60);
    return 1;
}
