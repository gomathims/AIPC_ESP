#include "event_log.h"

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
extern "C" {
#include <user_interface.h>   // struct rst_info (ESP.getResetInfoPtr()->reason)
}

#include "time_util.h"
#include "cloud_mqtt.h"   // cloudLogf() / cloudPublishEvent() — mirror to ThingsBoard

#define LOG_PATH      "/events.bin"
#define LOG_TMP_PATH  "/events.tmp"
#define LOG_MAX_BYTES 24576U    // ~4096 records; trimmed when exceeded
#define LOG_SHOW_MAX  150       // newest N records returned to the web UI

struct __attribute__((packed)) LogRec {
    uint32_t ts;   // local-wall epoch (0 if clock not yet set)
    uint8_t ev;    // MeterEvent
    uint8_t aux;   // event-specific (boot: reset reason 0..6)
};                 // 6 bytes

static bool fsReady = false;

// The boot record is captured at init (reset reason is only meaningful then) but
// written once so it can carry a real timestamp when the clock is available.
static uint8_t bootReason = 0;
static bool bootPending = true;

// ----------------------------------------------------------------------------
static const char *evName(uint8_t ev) {
    switch (ev) {
        case EV_BOOT:         return "Device restarted";
        case EV_R_MAIN:       return "R main relay";
        case EV_Y_MAIN:       return "Y main relay";
        case EV_B_MAIN:       return "B main relay";
        case EV_YR_AUX:       return "Y-R aux relay";
        case EV_BR_AUX:       return "B-R aux relay";
        case EV_RY_AUX:       return "R-Y aux relay";
        case EV_BY_AUX:       return "B-Y aux relay";
        case EV_RB_AUX:       return "R-B aux relay";
        case EV_YB_AUX:       return "Y-B aux relay";
        case EV_OVERLOAD_R:   return "R phase overload";
        case EV_OVERLOAD_Y:   return "Y phase overload";
        case EV_OVERLOAD_B:   return "B phase overload";
        case EV_STM_RESTART:  return "STM32 restarted";
        default:              return "Event";
    }
}

// Human-readable ESP8266 reset reason (matches the map in app.js).
static const char *resetReasonName(uint8_t rc) {
    switch (rc) {
        case 0: return "Power on";
        case 1: return "Hardware watchdog";
        case 2: return "Exception";
        case 3: return "Software watchdog";
        case 4: return "Software restart";
        case 5: return "Wake from deep sleep";
        case 6: return "External reset";
        default: return "Unknown";
    }
}

static void trimLog() {
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) return;
    size_t sz = f.size();
    size_t keep = LOG_MAX_BYTES / 2;
    keep -= keep % sizeof(LogRec);
    if (sz <= keep) { f.close(); return; }

    f.seek(sz - keep);
    File t = LittleFS.open(LOG_TMP_PATH, "w");
    if (!t) { f.close(); return; }
    uint8_t buf[256];
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        t.write(buf, n);
    }
    f.close();
    t.close();
    LittleFS.remove(LOG_PATH);
    LittleFS.rename(LOG_TMP_PATH, LOG_PATH);
}

static void appendRec(uint8_t ev, uint8_t aux, uint32_t ts) {
    if (!fsReady) return;

    LogRec rec;
    rec.ts = ts;
    rec.ev = ev;
    rec.aux = aux;

    bool written = false;
    File f = LittleFS.open(LOG_PATH, "a");
    if (f && f.size() >= LOG_MAX_BYTES) {
        f.close();
        trimLog();
        f = LittleFS.open(LOG_PATH, "a");
    }
    if (f) {
        written = (f.write((const uint8_t *)&rec, sizeof(rec)) == sizeof(rec));
        f.close();
    }

    // Mirror to the cloud only after the record is safely in flash.
    if (written) {
        if (ev == EV_BOOT) {
            cloudLogf("[BOOT] device restarted (%s)", resetReasonName(aux));
        }
        cloudPublishEvent(ev, evName(ev));
    }
}

// ----------------------------------------------------------------------------
void initEventLog() {
    fsReady = LittleFS.begin();

    // Capture the reset reason now (only valid right after boot). The boot record
    // is written by serviceEventLog() once the clock is set so it carries a real
    // timestamp; if the clock never gets set it is still written after a short
    // grace period so every restart is recorded regardless of connectivity.
    bootReason = (uint8_t)ESP.getResetInfoPtr()->reason;
    bootPending = true;
}

void logEvent(uint8_t ev, uint8_t aux) {
    appendRec(ev, aux, localWallEpoch());   // 0 if clock not set yet
}

void logEventAt(uint8_t ev, uint8_t aux, uint32_t ts) {
    appendRec(ev, aux, ts);
}

void serviceEventLog() {
    if (!fsReady || !bootPending) return;

    // Prefer a real timestamp; fall back to ts=0 after ~30 s if the clock never sets
    // (offline / never logged in) so the restart is still recorded.
    static unsigned long firstService = 0;
    if (firstService == 0) firstService = millis();

    if (localWallEpoch() != 0 || (millis() - firstService > 30000UL)) {
        bootPending = false;
        appendRec(EV_BOOT, bootReason, localWallEpoch());
    }
}

// ----------------------------------------------------------------------------
String getLogsJson() {
    JsonDocument doc;
    JsonArray arr = doc["logs"].to<JsonArray>();

    if (fsReady) {
        File f = LittleFS.open(LOG_PATH, "r");
        if (f) {
            size_t cnt = f.size() / sizeof(LogRec);
            size_t start = (cnt > LOG_SHOW_MAX) ? (cnt - LOG_SHOW_MAX) : 0;
            f.seek(start * sizeof(LogRec));
            std::vector<LogRec> recs;
            LogRec r;
            while (f.read((uint8_t *)&r, sizeof(r)) == sizeof(r)) recs.push_back(r);
            f.close();
            for (int i = (int)recs.size() - 1; i >= 0; i--) {
                JsonObject o = arr.add<JsonObject>();
                o["ts"] = recs[i].ts;
                o["ev"] = recs[i].ev;
                o["rc"] = recs[i].aux;
            }
        }
    }

    String out;
    serializeJson(doc, out);
    return out;
}

bool clearEventLog() {
    if (!fsReady) return false;
    LittleFS.remove(LOG_PATH);
    return true;
}
