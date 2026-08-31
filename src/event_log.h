#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <Arduino.h>

// Flash-backed operation log (LittleFS) for the 3-phase meter.
// Records device events as compact binary records and serves them to the web UI.
//
// For now the only logged event is BOOT / restart (carrying the ESP8266 reset
// reason: power-on, watchdog, exception, external, software). More event types
// (e.g. metering thresholds) can be added later via logEvent().

// Event codes.
enum MeterEvent : uint8_t {
    EV_BOOT = 0,      // device (re)started; aux = reset reason
    // 1.. reserved for future metering events
};

void initEventLog();        // mount LittleFS. Call once in setup().
void serviceEventLog();     // append the deferred boot record once the clock is set. Call from loop().
void logEvent(uint8_t ev, uint8_t aux);  // append an arbitrary event now.
String getLogsJson();       // {"logs":[{ts,ev,rc}, ...]} newest-first, capped.
bool clearEventLog();       // delete the log file.

#endif // EVENT_LOG_H
