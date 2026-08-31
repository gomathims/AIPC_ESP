#ifndef METER_STATE_H
#define METER_STATE_H

#include <Arduino.h>

// Live 3-phase metering state shared with the web portal / cloud.
// DEFINED in main.cpp; declared extern here so the WiFi/cloud/log layers can
// read them without duplicating definitions.
//
// Values are decoded from the STM32 UART frame (see uartCollection() in main.cpp):
//   voltLx  = line voltage  (Volts)
//   curLx   = line current  (Amps)
//   powLx   = active power   (Watts)
//   eneLx   = energy         (Wh / kWh as scaled by the STM front-end)
extern volatile float voltL1, curL1, powL1, eneL1;
extern volatile float voltL2, curL2, powL2, eneL2;
extern volatile float voltL3, curL3, powL3, eneL3;

// True once at least one valid STM frame has been decoded; millis() of the last.
extern volatile bool meterDataValid;
extern volatile unsigned long meterLastUpdate;

#endif // METER_STATE_H
