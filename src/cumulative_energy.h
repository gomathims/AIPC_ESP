#ifndef CUMULATIVE_ENERGY_H
#define CUMULATIVE_ENERGY_H

#include <Arduino.h>

// Cumulative (lifetime) energy per channel, persisted to flash.
//
// The STM32 reports each BL0939 energy counter (eneL1..eneL4), which restarts
// from 0 whenever the STM32 or the BL0939 resets. Each frame we add the increase
// since the previous frame to a running total; if the counter went backwards
// (reset), the new reading itself is the increase. Totals are written to flash
// once per CUM_WRITE_TIME (1 hour) to limit flash wear.
//
// Index: 0 = L1 (R), 1 = L2 (Y), 2 = L3 (B), 3 = L4 (Inverter).

#define CUM_CHANNELS 4
#define CUM_WRITE_TIME 3600000UL   // 1 hour, same as the EVIQ-3PH firmware

extern float cumEnergy[CUM_CHANNELS];   // kWh

void initCumulativeEnergy();     // load saved totals. Call once in setup().
void updateCumulativeEnergy();   // call after every decoded STM metering frame.
void saveCumulativeEnergy();     // write totals to flash now (e.g. before a reboot).
void resetCumulativeEnergy();    // set all totals to 0 and save; counting restarts from zero.

#endif // CUMULATIVE_ENERGY_H
