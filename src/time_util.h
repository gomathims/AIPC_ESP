#ifndef TIME_UTIL_H
#define TIME_UTIL_H

#include <Arduino.h>
#include <time.h>

// Shared time helpers for the ESP8266 port.
//
// The clock is kept on LOCAL time via configTime(GMT_OFFSET, 0, ...), so
// localtime_r() yields local wall-clock. The web UI stores a "local epoch"
// (local time encoded as if it were UTC) and formats with UTC getters, which
// avoids any timezone double-conversion in the browser.

// India Standard Time (UTC+5:30) baked in as the device-local offset.
#define GMT_OFFSET_SEC (5 * 3600 + 1800)
#define DST_OFFSET_SEC 0
#define TS_VALID_MIN 1700000000UL   // epoch sanity floor (after 2023-11)



/*Variable declearation*/
extern bool timesynced;

// Fill *out with local broken-down time. Returns false if the clock is unset
// (year < 2020). ESP8266-friendly replacement for ESP32's getLocalTime().
static inline bool localTimeNow(struct tm *out) {
    time_t now = time(nullptr);
    if (now < (time_t)TS_VALID_MIN) return false;
    localtime_r(&now, out);
    return (out->tm_year >= 120);   // year >= 2020
}


/*Pratise function for STM RTC time as the master TIME*/
// static inline bool localTimeNow(struct tm *out){

// }

// Portable timegm(): interpret a broken-down time as UTC and return the epoch.
static inline time_t tmToUtcEpoch(const struct tm *t) {
    int year = t->tm_year + 1900;
    int month = t->tm_mon + 1;   // 1..12
    int day = t->tm_mday;
    year -= (month <= 2);
    long era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

// Current wall-clock as a "local epoch" (local time encoded as UTC), or 0 if
// the clock has not been set yet.
static inline uint32_t localWallEpoch() {
    struct tm t;
    if (!localTimeNow(&t)) return 0;
    time_t e = tmToUtcEpoch(&t);
    return (e > (time_t)TS_VALID_MIN) ? (uint32_t)e : 0;
}

#endif // TIME_UTIL_H
