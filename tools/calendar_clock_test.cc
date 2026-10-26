// 验证生产代码的日期校验与下一次零点计算，覆盖 RTC 支持的全部日期。
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "calendar_clock.h"

int main() {
    _putenv_s("TZ", "CST-8");
    _tzset();
    int dates = 0;
    for (int year = 2000; year <= 2099; ++year) {
        for (int month = 1; month <= 12; ++month) {
            for (int day = 1; day <= cal_days_in_month(year, month); ++day) {
                tm now = {};
                now.tm_year = year - 1900;
                now.tm_mon = month - 1;
                now.tm_mday = day;
                now.tm_hour = 23;
                now.tm_min = 59;
                now.tm_sec = 59;
                assert(CalendarTimeValid(now));
                tm next = {};
                if (year == 2099 && month == 12 && day == 31) {
                    assert(!CalendarNextMidnight(now, &next));
                    continue;
                }
                assert(CalendarNextMidnight(now, &next));
                assert(next.tm_hour == 0 && next.tm_min == 0 && next.tm_sec == 0);
                const cal_date current = {year, month, day};
                const cal_date following = {next.tm_year + 1900, next.tm_mon + 1,
                                            next.tm_mday};
                cal_lunar lunar;
                assert(cal_lunar_from_solar(&following, &lunar));
                assert(cal_weekday(following.year, following.month, following.day) ==
                       (cal_weekday(current.year, current.month, current.day) + 1) % 7);
                assert(!CalendarSameDate(now, next));
                const time_t before = std::mktime(&now);
                assert(std::mktime(&next) - before == 1);
                now.tm_hour = now.tm_min = now.tm_sec = 0;
                tm same_target = {};
                assert(CalendarNextMidnight(now, &same_target));
                assert(CalendarSameDate(same_target, next));
                assert(std::mktime(&next) - std::mktime(&now) == 86400);
                ++dates;
            }
        }
    }
    tm invalid = {};
    invalid.tm_year = 126;
    invalid.tm_mon = 1;
    invalid.tm_mday = 29;
    assert(!CalendarTimeValid(invalid));
    invalid.tm_year = 128;
    assert(CalendarTimeValid(invalid));
    invalid.tm_hour = 24;
    assert(!CalendarTimeValid(invalid));
    invalid.tm_hour = 0;
    invalid.tm_min = 60;
    assert(!CalendarTimeValid(invalid));
    invalid.tm_min = 0;
    invalid.tm_sec = 60;
    assert(!CalendarTimeValid(invalid));
    invalid.tm_sec = 0;
    invalid.tm_mon = 12;
    assert(!CalendarTimeValid(invalid));
    invalid.tm_mon = 1;
    invalid.tm_year = 99;
    assert(!CalendarTimeValid(invalid));
    std::printf("calendar clock: %d dates, midnight + 23:59:59, invalid inputs OK\n", dates);
}
