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
    EV_BOOT = 0,          // device (re)started; aux = reset reason
    // STM32 front-end relay/overload bits. aux: 1 = ON / overload present, 0 = OFF / cleared.
    EV_R_MAIN = 1,        // R phase main relay
    EV_Y_MAIN = 2,        // Y phase main relay
    EV_B_MAIN = 3,        // B phase main relay
    EV_YR_AUX = 4,        // Y-R aux relay
    EV_BR_AUX = 5,        // B-R aux relay
    EV_RY_AUX = 6,        // R-Y aux relay
    EV_BY_AUX = 7,        // B-Y aux relay
    EV_RB_AUX = 8,        // R-B aux relay
    EV_YB_AUX = 9,        // Y-B aux relay
    EV_OVERLOAD_R = 10,   // R phase overload
    EV_OVERLOAD_Y = 11,   // Y phase overload
    EV_OVERLOAD_B = 12,   // B phase overload
    EV_STM_RESTART = 13,  // STM32 front-end restarted; aux unused
};

void initEventLog();        // mount LittleFS. Call once in setup().
void serviceEventLog();     // append the deferred boot record once the clock is set. Call from loop().
void logEvent(uint8_t ev, uint8_t aux);                    // append now, timestamped with the local clock.
void logEventAt(uint8_t ev, uint8_t aux, uint32_t ts);      // append with an explicit timestamp (e.g. from the STM32).
String getLogsJson();       // {"logs":[{ts,ev,rc}, ...]} newest-first, capped.
bool clearEventLog();       // delete the log file.

#endif // EVENT_LOG_H
