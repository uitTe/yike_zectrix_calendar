#ifndef CALENDAR_CLOCK_H
#define CALENDAR_CLOCK_H

#include <ctime>

#include "cal_almanac.h"

inline bool CalendarTimeValid(const tm& value) {
    return value.tm_year >= 100 && value.tm_year <= 199 &&
           value.tm_mon >= 0 && value.tm_mon < 12 && value.tm_mday >= 1 &&
           value.tm_mday <= cal_days_in_month(value.tm_year + 1900, value.tm_mon + 1) &&
           value.tm_hour >= 0 && value.tm_hour < 24 &&
           value.tm_min >= 0 && value.tm_min < 60 &&
           value.tm_sec >= 0 && value.tm_sec < 60;
}

inline bool CalendarSameDate(const tm& first, const tm& second) {
    return first.tm_year == second.tm_year && first.tm_mon == second.tm_mon &&
           first.tm_mday == second.tm_mday;
}

inline bool CalendarNextMidnight(const tm& now, tm* next) {
    if (!CalendarTimeValid(now)) {
        return false;
    }
    *next = now;
    next->tm_hour = 0;
    next->tm_min = 0;
    next->tm_sec = 0;
    next->tm_mday += 1;
    next->tm_isdst = -1;
    return std::mktime(next) != static_cast<time_t>(-1) && CalendarTimeValid(*next);
}

#endif
