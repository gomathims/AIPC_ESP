#ifndef METER_STATE_H
#define METER_STATE_H

#include <Arduino.h>
#include <time.h>

// Live 3-phase metering state shared with the web portal / cloud.
// DEFINED in main.cpp; declared extern here so the WiFi/cloud/log layers can
// read them without duplicating definitions.
//
// Values are decoded from the STM32 UART frame (see uartCollection() in main.cpp):
//   voltLx  = line voltage  (Volts)
//   curLx   = line current  (Amps)
//   powLx   = active power   (Watts)
//   eneLx   = energy         (Wh / kWh as scaled by the STM front-end)
// Sent by the STM as raw little-endian floats (already calibrated), copied with memcpy.
extern float voltL1, curL1, powL1, eneL1;
extern float voltL2, curL2, powL2, eneL2;
extern float voltL3, curL3, powL3, eneL3;
// 4th BL0939 channel: the inverter (shown as "Inverter" on the home page).
extern float voltL4, curL4, powL4, eneL4;
extern volatile uint16 relayStatus;

// True once at least one valid STM frame has been decoded; millis() of the last.
extern volatile bool meterDataValid;
extern volatile unsigned long meterLastUpdate;

// Battery voltage (Volts) reported by the STM32 front-end.
extern float bateryVoltag;

// STM32 RTC time (unix epoch) from the last metering frame; 0 until one arrives.
extern time_t curTime;

// Contactor feedback bits from the STM32 frame byte [58]: bit0 = CF1 ... bit6 = CF7.
extern uint8_t contactorStatus;

// Relay states from the STM32 frame bytes [48..49]: bit0 R main, 1 Y main, 2 B main,
// 3 R->Y aux, 4 Y->R aux, 5 Y->B aux, 6 B->Y aux, 7 R->B aux, 8 B->R aux, 9 UPS main, 10 UPS aux.
extern volatile uint16 relayStatus;

// Active protection alarms from the STM32 event flag (bits 11..15), 1 = present:
//   bit0 R overload, bit1 Y overload, bit2 B overload, bit3 Inverter overload,
//   bit4 Inverter (UPS) over power. Updated on every STM event frame (0xEE).
extern volatile uint8_t stmAlarm;

#endif // METER_STATE_H
