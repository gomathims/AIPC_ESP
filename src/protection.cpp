#include "protection.h"

#include "prefs_store.h"    // LittleFS-backed Preferences shim
#include "meter_state.h"

static Preferences protPrefs;
static const char *PROT_NS = "prot";

// Defaults (typical 230 V single-phase limits; user-adjustable on the web page).
PhaseProtection prot[3] = {
    {false, 250.0f, 180.0f, 20.0f},
    {false, 250.0f, 180.0f, 20.0f},
    {false, 250.0f, 180.0f, 20.0f},
};

static void liveVI(int i, float &v, float &cur) {
    switch (i) {
        case 0: v = voltL1; cur = curL1; break;
        case 1: v = voltL2; cur = curL2; break;
        default: v = voltL3; cur = curL3; break;
    }
}

static const char *phaseStatus(int i) {
    if (!prot[i].enabled) return "off";
    if (!meterDataValid) return "—";
    float v, cur;
    liveVI(i, v, cur);
    if (v > prot[i].highV) return "HIGH VOLTAGE";
    if (v < prot[i].lowV) return "LOW VOLTAGE";
    if (cur > prot[i].highI) return "OVERLOAD";
    return "OK";
}

void initProtection() {
    protPrefs.begin(PROT_NS, /*readOnly=*/true);
    String cfg = protPrefs.getString("cfg", "");
    protPrefs.end();

    if (cfg.length()) {
        JsonDocument doc;
        if (!deserializeJson(doc, cfg) && doc["phases"].is<JsonArray>()) {
            JsonArray arr = doc["phases"].as<JsonArray>();
            for (int i = 0; i < 3 && i < (int)arr.size(); i++) {
                prot[i].enabled = arr[i]["en"] | prot[i].enabled;
                prot[i].highV = arr[i]["hv"] | prot[i].highV;
                prot[i].lowV = arr[i]["lv"] | prot[i].lowV;
                prot[i].highI = arr[i]["hi"] | prot[i].highI;
            }
        }
    }
}

String getProtectionJson() {
    JsonDocument doc;
    JsonArray arr = doc["phases"].to<JsonArray>();
    for (int i = 0; i < 3; i++) {
        float v, cur;
        liveVI(i, v, cur);
        JsonObject o = arr.add<JsonObject>();
        o["en"] = prot[i].enabled;
        o["hv"] = prot[i].highV;
        o["lv"] = prot[i].lowV;
        o["hi"] = prot[i].highI;
        o["v"] = round(v * 10.0) / 10.0;
        o["i"] = round(cur * 100.0) / 100.0;
        o["status"] = phaseStatus(i);
    }
    String out;
    serializeJson(doc, out);
    return out;
}

bool saveProtectionFromJson(JsonDocument &doc) {
    if (!doc["phases"].is<JsonArray>()) return false;
    JsonArray arr = doc["phases"].as<JsonArray>();
    if (arr.size() < 3) return false;

    PhaseProtection tmp[3];
    for (int i = 0; i < 3; i++) {
        tmp[i].enabled = arr[i]["en"] | false;
        tmp[i].highV = arr[i]["hv"] | 0.0f;
        tmp[i].lowV = arr[i]["lv"] | 0.0f;
        tmp[i].highI = arr[i]["hi"] | 0.0f;
        // Only enforce limits on phases the user actually enabled.
        // Ranges must match the web UI: Low 150-250 V, High 200-290 V (>Low), Overload 0.01-50 A.
        if (tmp[i].enabled) {
            if (tmp[i].lowV < 150.0f || tmp[i].lowV > 250.0f) return false;
            if (tmp[i].highV < 200.0f || tmp[i].highV > 290.0f) return false;
            if (tmp[i].highV <= tmp[i].lowV) return false;
            if (tmp[i].highI < 0.01f || tmp[i].highI > 50.0f) return false;
        }
    }
    for (int i = 0; i < 3; i++) prot[i] = tmp[i];

    JsonDocument save;
    JsonArray sarr = save["phases"].to<JsonArray>();
    for (int i = 0; i < 3; i++) {
        JsonObject o = sarr.add<JsonObject>();
        o["en"] = prot[i].enabled;
        o["hv"] = prot[i].highV;
        o["lv"] = prot[i].lowV;
        o["hi"] = prot[i].highI;
    }
    String s;
    serializeJson(save, s);

    protPrefs.begin(PROT_NS, /*readOnly=*/false);
    protPrefs.putString("cfg", s);
    protPrefs.end();
    protectionDetailsSaved=true;
    return true;
}

bool protectionTripped() {
    for (int i = 0; i < 3; i++) {
        const char *s = phaseStatus(i);
        if (strcmp(s, "OK") != 0 && strcmp(s, "off") != 0 && strcmp(s, "—") != 0) {
            return true;
        }
    }
    return false;
}
