#ifndef PROTECTION_H
#define PROTECTION_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Per-phase protection limits for the 3-phase meter.
//
// For each phase (L1/L2/L3) the user can enable protection and set:
//   - High Voltage limit  (flag when V rises above)
//   - Low Voltage limit   (flag when V drops below)
//   - Overload Current     (flag when I rises above)
//
// Limits are persisted to LittleFS. The live per-phase status (OK / HIGH VOLTAGE
// / LOW VOLTAGE / OVERLOAD / off) is computed against the current meter readings
// and returned to the web UI. Actual trip/relay action can be wired later once an
// output GPIO is defined.

struct PhaseProtection {
    bool enabled;
    float highV;   // Volts
    float lowV;    // Volts
    float highI;   // Amps (overload)
};

extern PhaseProtection prot[3];
extern bool protectionDetailsSaved;


void initProtection();                            // load saved limits. Call once in setup().
String getProtectionJson();                       // limits + live value + status per phase.
bool saveProtectionFromJson(JsonDocument &doc);   // validate + persist. false on invalid input.

// True if any enabled phase is currently outside its limits (for future relay use).
bool protectionTripped();

#endif // PROTECTION_H
