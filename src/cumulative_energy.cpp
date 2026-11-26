#include "cumulative_energy.h"

#include "prefs_store.h"    // LittleFS-backed Preferences shim
#include "meter_state.h"

static Preferences cumPrefs;
static const char *CUM_NS = "energy-data";
static const char *CUM_KEYS[CUM_CHANNELS] = {"cumR", "cumY", "cumB", "cumInv"};

// Ignore impossible jumps from a corrupted UART frame: 1 kWh between two frames
// (~1 s apart) would be several MW, far beyond what this meter can see.
static const float CUM_MAX_STEP_KWH = 1.0f;

float cumEnergy[CUM_CHANNELS] = {0, 0, 0, 0};
static float preCumEnergy[CUM_CHANNELS];
static bool firstRead = true;
static unsigned long lastWriteMs = 0;

void initCumulativeEnergy() {
    cumPrefs.begin(CUM_NS, /*readOnly=*/true);
    for (int i = 0; i < CUM_CHANNELS; i++) {
        cumEnergy[i] = cumPrefs.getFloat(CUM_KEYS[i], 0.0f);
    }
    cumPrefs.end();
    lastWriteMs = millis();
}

void saveCumulativeEnergy() {
    cumPrefs.begin(CUM_NS, /*readOnly=*/false);
    for (int i = 0; i < CUM_CHANNELS; i++) {
        cumPrefs.putFloat(CUM_KEYS[i], cumEnergy[i]);
    }
    cumPrefs.end();
    lastWriteMs = millis();
}

void resetCumulativeEnergy() {
    // Zero the totals. The baseline (preCumEnergy) is kept, so counting restarts
    // from 0 using the STM's current energy counters as the new starting point.
    for (int i = 0; i < CUM_CHANNELS; i++) cumEnergy[i] = 0.0f;
    saveCumulativeEnergy();
}

void updateCumulativeEnergy() {
    const float now[CUM_CHANNELS] = {eneL1, eneL2, eneL3, eneL4};

    if (firstRead) {
        // First frame after boot: only take the baseline.
        for (int i = 0; i < CUM_CHANNELS; i++) preCumEnergy[i] = now[i];
        firstRead = false;
        return;
    }

    for (int i = 0; i < CUM_CHANNELS; i++) {
        float step = now[i] - preCumEnergy[i];
        if (step < 0) step = now[i];   // counter restarted (STM32/BL0939 reset)
        if (step <= CUM_MAX_STEP_KWH) cumEnergy[i] += step;
        preCumEnergy[i] = now[i];
    }

    if (millis() - lastWriteMs > CUM_WRITE_TIME) {
        saveCumulativeEnergy();
    }
}
