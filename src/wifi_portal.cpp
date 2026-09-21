#include "wifi_portal.h"

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoJson.h>
#include <Updater.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>
#include <esp8266_peri.h>   // RANDOM_REG32 (hardware RNG)

#include "preference_management.h"
#include <LittleFS.h>

#include "meter_state.h"
#include "stm_ota.h"
#include "cloud_mqtt.h"
#include "event_log.h"
#include "protection.h"
#include "time_util.h"

// web_content.h / web_mappings.h are generated at build time by webgen.py from
// data/. web_mappings.h DEFINES the maps (stringMap / binaryMap), so it must be
// included in exactly ONE translation unit.
#include "web_mappings.h"

// ----------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_SUBNET(255, 255, 255, 0);

#define STA_CONNECT_TIMEOUT_MS 15000UL
#define STA_POLL_INTERVAL_MS     500UL
#define AP_RETRY_INTERVAL_MS   30000UL
#define WIFI_CHECK_INTERVAL_MS 15000UL

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------
static ESP8266WebServer server(80);

// 'A' = AP fallback, 'C' = connecting to STA, 'W' = connected to STA
static char wiFiMode = 'A';
static bool timeSynced = false;

static unsigned long staConnectStart = 0;
static unsigned long lastStaPoll = 0;
static unsigned long lastServiceTick = 0;

// OTA (web upload): reboot is deferred out of the request handler.
static volatile bool otaRebootPending = false;
static unsigned long otaRebootAt = 0;
static bool otaAuthOk = false;

static String apName() { return String(deviceName) + "_AP"; }

// ----------------------------------------------------------------------------
// WebSession / PIN authentication
// ----------------------------------------------------------------------------
#define MAX_SESSIONS 4
#define SESSION_TTL_MS (30UL * 60UL * 1000UL)
#define SESSION_COOKIE "eviqsid"

struct WebSession {
    uint32_t token;
    unsigned long expiry;
    bool active;
};
static WebSession webSessions[MAX_SESSIONS];

static uint32_t randomToken() {
    uint32_t t = RANDOM_REG32;   // ESP8266 hardware RNG
    return t ? t : 1;
}

static uint32_t createSession() {
    unsigned long now = millis();
    int slot = 0;
    unsigned long oldest = ULONG_MAX;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!webSessions[i].active || now > webSessions[i].expiry) { slot = i; break; }
        if (webSessions[i].expiry < oldest) { oldest = webSessions[i].expiry; slot = i; }
    }
    webSessions[slot].token = randomToken();
    webSessions[slot].expiry = now + SESSION_TTL_MS;
    webSessions[slot].active = true;
    return webSessions[slot].token;
}

static bool tokenValid(uint32_t tok) {
    if (tok == 0) return false;
    unsigned long now = millis();
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (webSessions[i].active && webSessions[i].token == tok && now <= webSessions[i].expiry) {
            webSessions[i].expiry = now + SESSION_TTL_MS;
            return true;
        }
    }
    return false;
}

static void invalidateToken(uint32_t tok) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (webSessions[i].token == tok) webSessions[i].active = false;
    }
}

static uint32_t cookieToken() {
    if (!server.hasHeader("Cookie")) return 0;
    String c = server.header("Cookie");
    int i = c.indexOf(SESSION_COOKIE "=");
    if (i < 0) return 0;
    i += strlen(SESSION_COOKIE) + 1;
    int e = c.indexOf(';', i);
    String v = (e < 0) ? c.substring(i) : c.substring(i, e);
    v.trim();
    return (uint32_t)strtoul(v.c_str(), nullptr, 10);
}

static bool isAuthed() { return tokenValid(cookieToken()); }

static bool requireAuth() {
    if (!isAuthed()) {
        server.send(401, "text/plain", "Unauthorized");
        return false;
    }
    return true;
}

// Parse the JSON body of the current request. Returns false (and sends 400) on error.
static bool readJsonBody(JsonDocument &doc) {
    String body = server.arg("plain");
    if (deserializeJson(doc, body)) {
        server.send(400, "text/plain", "Invalid JSON");
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// Time (system clock + NTP)
// ----------------------------------------------------------------------------
static void setRtcFromEpoch(uint32_t localEpoch) {
    if (localEpoch < TS_VALID_MIN) return;
    // Clock is read as local time (TZ=GMT_OFFSET). Store UTC = localWall - offset so
    // localtime() yields the browser's wall-clock even before NTP syncs.
    struct timeval tv;
    tv.tv_sec = (time_t)localEpoch - GMT_OFFSET_SEC;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    timesynced = true;
}

static void startNtpSync() {
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, "pool.ntp.org", "time.nist.gov", "time.google.com");
    timesynced=true;
}

static String getDeviceTimeString() {
    struct tm t;
    if (!localTimeNow(&t)) return String("");
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    return String(buf);
}

// ----------------------------------------------------------------------------
// Static file serving (from PROGMEM maps in web_mappings.h)
// ----------------------------------------------------------------------------
static String getMimeType(const String &path) {
    if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
    if (path.endsWith(".css")) return "text/css";
    if (path.endsWith(".js")) return "application/javascript";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
    if (path.endsWith(".ico")) return "image/x-icon";
    return "text/plain";
}

static void handleStaticRequest() {
    String path = server.uri();
    if (path == "/" || path.isEmpty()) path = "/index.html";
    if (path.startsWith("/")) path = path.substring(1);

    // Dashboard + log require login; everything else is public so login can render.
    if ((path == "index.html" || path == "log.html") && !isAuthed()) {
        path = "login.html";
    }

    auto txt = stringMap.find(path);
    if (txt != stringMap.end()) {
        server.send_P(200, getMimeType(path).c_str(), txt->second);
        return;
    }

    auto bin = binaryMap.find(path);
    if (bin != binaryMap.end()) {
        server.sendHeader("Cache-Control", "max-age=31536000, public");
        server.send_P(200, getMimeType(path).c_str(), (const char *)bin->second.data, bin->second.length);
        return;
    }

    server.send(404, "text/html", "<h1>404 Not Found</h1>");
}

// ----------------------------------------------------------------------------
// JSON endpoints
// ----------------------------------------------------------------------------
static String buildReadingsJson() {
    JsonDocument doc;
    doc["v1"] = round(voltL1 * 10.0) / 10.0;
    doc["v2"] = round(voltL2 * 10.0) / 10.0;
    doc["v3"] = round(voltL3 * 10.0) / 10.0;
    doc["i1"] = round(curL1 * 100.0) / 100.0;
    doc["i2"] = round(curL2 * 100.0) / 100.0;
    doc["i3"] = round(curL3 * 100.0) / 100.0;
    doc["p1"] = round(powL1 * 10.0) / 10.0;
    doc["p2"] = round(powL2 * 10.0) / 10.0;
    doc["p3"] = round(powL3 * 10.0) / 10.0;
    doc["e1"] = round(eneL1 * 1000.0) / 1000.0;
    doc["e2"] = round(eneL2 * 1000.0) / 1000.0;
    doc["e3"] = round(eneL3 * 1000.0) / 1000.0;
    doc["batt"] = round(bateryVoltag * 10.0) / 10.0;
    // doc["valid"] = (bool)meterDataValid;
    doc["time"] = getDeviceTimeString();
    doc["timeSynced"] = timeSynced;

    String out;
    serializeJson(doc, out);
    return out;
}

static String buildWiFiStatusJson() {
    JsonDocument doc;
    doc["deviceName"] = deviceName;
    doc["wifiConfigured"] = isWiFiSet;
    doc["savedSSID"] = ssidBuffer;

    if (wiFiMode == 'W' && WiFi.status() == WL_CONNECTED) {
        doc["mode"] = "WIFI";
        doc["ssid"] = WiFi.SSID();
        doc["ip"] = WiFi.localIP().toString();
        doc["rssi"] = WiFi.RSSI();
    } else {
        doc["mode"] = "AP";
        doc["ssid"] = apName();
        doc["ip"] = WiFi.softAPIP().toString();
    }
    WiFiMode_t m = WiFi.getMode();
    doc["apActive"] = (m == WIFI_AP || m == WIFI_AP_STA);
    doc["apSSID"] = apName();
    doc["apIP"] = WiFi.softAPIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["mdns"] = String(deviceName) + ".local";

    String out;
    serializeJson(doc, out);
    return out;
}

// ---- POST handlers (sync; body via server.arg("plain")) --------------------
static void onSetWiFi() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Incorrect PIN"); return; }
    const char *ssid = doc["ssid"] | "";
    const char *pass = doc["pass"] | "";
    if (strlen(ssid) == 0 || strlen(ssid) > 32 || strlen(pass) > 64) {
        server.send(400, "text/plain", "SSID required (<=32 chars), password <=64 chars");
        return;
    }
    saveWifiCreds(ssid, pass);
    server.send(200, "text/plain", "WiFi saved. Connecting...");
}

static void onSetDeviceName() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Incorrect PIN"); return; }
    if (!saveDeviceName(doc["name"] | "")) {
        server.send(400, "text/plain", "Name must be 4-20 characters");
        return;
    }
    server.send(200, "text/plain", "Name saved. Restart to apply AP/mDNS name.");
}

static void onSetAPPassword() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Incorrect PIN"); return; }
    if (!saveAPPassword(doc["pass"] | "")) {
        server.send(400, "text/plain", "AP password must be >=8 characters (or blank to disable)");
        return;
    }
    server.send(200, "text/plain", "AP password saved. Restart to apply.");
}

static void onSetPin() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Current PIN is incorrect"); return; }
    if (!saveDevicePin(doc["pin"] | "")) { server.send(400, "text/plain", "New PIN must be 4 digits"); return; }
    server.send(200, "text/plain", "PIN updated.");
}

static void onConnectCloud() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Incorrect PIN"); return; }
    const char *email = doc["email"] | "";
    if (strlen(email) < 5 || strchr(email, '@') == nullptr) {
        server.send(400, "text/plain", "Enter a valid email address.");
        return;
    }
    if (!portalWiFiConnected()) {
        server.send(400, "text/plain", "Please connect the device to WiFi first.");
        return;
    }
    cloudRequestConnect(String(email));
    server.send(200, "text/plain", "Registering with cloud… watch the status below.");
}

static void onPauseCloud() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Incorrect PIN"); return; }
    cloudDisconnect();
    server.send(200, "text/plain", "Successfully disconnected from the cloud");
}

static void onDisableWiFi() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Incorrect PIN"); return; }
    clearWifiCreds();
    server.send(200, "text/plain", "WiFi disconnected. Switched to Access Point mode.");
}

static void onLogout() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Incorrect PIN"); return; }
    invalidateToken(cookieToken());
    server.sendHeader("Set-Cookie", String(SESSION_COOKIE) + "=; Path=/; Max-Age=0");
    server.send(200, "text/plain", "OK");
}

static void onClearLogs() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Incorrect PIN"); return; }
    clearEventLog();
    server.send(200, "text/plain", "Logs cleared.");
}

static void onSetProtection() {
    if (!requireAuth()) return;
    JsonDocument doc; if (!readJsonBody(doc)) return;
    if (!verifyDevicePin(doc["authPin"] | "")) { server.send(403, "text/plain", "Incorrect PIN"); return; }
    if (!saveProtectionFromJson(doc)) {
        server.send(400, "text/plain", "Invalid limits: High Voltage must be greater than Low Voltage, and Overload current > 0.");
        return;
    }
    server.send(200, "text/plain", "Protection settings saved.");
}

static void onLogin() {
    timesynced = true;
    JsonDocument doc;
    String body = server.arg("plain");
    if (deserializeJson(doc, body)) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid request\"}");
        return;
    }
    if (!verifyDevicePin(doc["devicePin"] | "")) {
        server.send(200, "application/json", "{\"success\":false,\"message\":\"Incorrect PIN\"}");
        return;
    }
    // Seed the clock from the browser if it isn't set yet (offline / no NTP).
    struct tm chk;
    if (!localTimeNow(&chk)) {
        setRtcFromEpoch((uint32_t)(doc["localEpoch"] | 0));
    }
    uint32_t tok = createSession();
    String cookie = String(SESSION_COOKIE) + "=" + String(tok) + "; Path=/; Max-Age=1800; SameSite=Lax";
    server.sendHeader("Set-Cookie", cookie);
    server.send(200, "application/json", "{\"success\":true}");

}

static void onScanNetworks() {
    if (!requireAuth()) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        server.send(200, "application/json", "{\"status\":\"scanning\"}");
        return;
    }
    if (n == WIFI_SCAN_FAILED) {
        WiFi.scanNetworks(true);   // async
        server.send(200, "application/json", "{\"status\":\"scanning\"}");
        return;
    }
    JsonDocument doc;
    doc["status"] = "done";
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["secure"] = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
    }
    String out;
    serializeJson(doc, out);
    WiFi.scanDelete();
    server.send(200, "application/json", out);
}

// ---- OTA firmware upload (multipart, field "firmware") ---------------------
static void handleOtaUpload() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        bool pinOk = server.hasArg("authPin") && verifyDevicePin(server.arg("authPin").c_str());
        otaAuthOk = isAuthed() && pinOk;
        if (!otaAuthOk) { Serial.println("[OTA] unauthorized upload rejected"); return; }
        uint32_t maxSketch = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
        if (!Update.begin(maxSketch)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!otaAuthOk) return;
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (!otaAuthOk) return;
        if (Update.end(true)) {
            Serial.printf("[OTA] success: %u bytes\n", up.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
    yield();
}

// ---- STM32 firmware upload (multipart, field "firmware") -------------------
// Staged to LittleFS first, then flashed over the metering UART once the whole
// image has arrived — the AN3155 transfer cannot be interleaved with the HTTP
// upload because it needs exclusive use of the port.
#define STM_FW_PATH "/stm32fw.bin"

static bool stmUploadAuthOk = false;
static File stmFwFile;

static void handleSTM32Upload() {
    HTTPUpload &up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        bool pinOk = server.hasArg("authPin") && verifyDevicePin(server.arg("authPin").c_str());
        stmUploadAuthOk = isAuthed() && pinOk;
        if (!stmUploadAuthOk) return;
        if (stmFwFile) stmFwFile.close();   // a previous upload that never reached _END
        LittleFS.remove(STM_FW_PATH);
        stmFwFile = LittleFS.open(STM_FW_PATH, "w");
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!stmUploadAuthOk || !stmFwFile) return;
        stmFwFile.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        if (stmFwFile) stmFwFile.close();
    }
    yield();
}

static void onSTM32UploadFinished() {
    if (!stmUploadAuthOk) {
        server.send(403, "text/plain", "Incorrect PIN or unauthorized");
        return;
    }
    // Blocking: handshake, switch to 8E1, erase, stream, GO, restore 8N1.
    StmOtaResult r = stmOtaFlashFromFile(STM_FW_PATH);
    LittleFS.remove(STM_FW_PATH);   // don't leave a stale image in the filesystem

    server.sendHeader("Connection", "close");
    server.send(r == STM_OTA_OK ? 200 : 500, "text/plain", stmOtaResultName(r));
}

static void onOtaFinished() {
    bool ok = otaAuthOk && !Update.hasError();
    if (!otaAuthOk) {
        server.send(403, "text/plain", "Incorrect PIN or unauthorized");
        return;
    }
    server.sendHeader("Connection", "close");
    server.send(ok ? 200 : 500, "text/plain", ok ? "Update OK" : "Update failed");
    if (ok) { otaRebootPending = true; otaRebootAt = millis(); }
}

static void registerRoutes() {
    server.on("/api/getReadings", HTTP_GET, []() {
        if (!requireAuth()) return;
        server.send(200, "application/json", buildReadingsJson());
    });
    server.on("/api/getWiFiStatus", HTTP_GET, []() {
        if (!requireAuth()) return;
        server.send(200, "application/json", buildWiFiStatusJson());
    });
    server.on("/api/deviceName", HTTP_GET, []() {
        server.send(200, "text/plain", deviceName);
    });
    server.on("/api/scanNetworks", HTTP_GET, onScanNetworks);
    server.on("/api/getLogs", HTTP_GET, []() {
        if (!requireAuth()) return;
        server.send(200, "application/json", getLogsJson());
    });
    server.on("/api/clearLogs", HTTP_POST, onClearLogs);
    server.on("/api/getProtection", HTTP_GET, []() {
        if (!requireAuth()) return;
        server.send(200, "application/json", getProtectionJson());
    });
    server.on("/api/setProtection", HTTP_POST, onSetProtection);
    server.on("/api/getCloudStatus", HTTP_GET, []() {
        if (!requireAuth()) return;
        server.send(200, "application/json", getCloudStatusJson());
    });
    server.on("/api/connectCloud", HTTP_POST, onConnectCloud);
    server.on("/api/pauseCloud", HTTP_POST, onPauseCloud);
    server.on("/api/login", HTTP_POST, onLogin);
    server.on("/api/logout", HTTP_POST, onLogout);
    server.on("/api/disableWiFi", HTTP_POST, onDisableWiFi);
    server.on("/api/setWiFi", HTTP_POST, onSetWiFi);
    server.on("/api/setDeviceName", HTTP_POST, onSetDeviceName);
    server.on("/api/setAPPassword", HTTP_POST, onSetAPPassword);
    server.on("/api/setPin", HTTP_POST, onSetPin);

    server.on("/updateSystem", HTTP_POST, onOtaFinished, handleOtaUpload);
    server.on("/updateSTM32", HTTP_POST, onSTM32UploadFinished, handleSTM32Upload);
    // Stage 2: install what stage 1 staged into the front-end's bank 2.
    server.on("/applySTM32", HTTP_POST, []() {
        if (!requireAuth()) return;
        if (!server.hasArg("authPin") || !verifyDevicePin(server.arg("authPin").c_str())) {
            server.send(403, "text/plain", "Incorrect PIN");
            return;
        }
        StmOtaResult r = stmOtaApply();
        server.sendHeader("Connection", "close");
        server.send(r == STM_OTA_OK ? 200 : 500, "text/plain", stmOtaResultName(r));
    });
    server.on("/api/getSTM32OtaStatus", HTTP_GET, []() {
        if (!requireAuth()) return;
        JsonDocument doc;
        doc["inProgress"] = (bool)stmOtaInProgress;
        doc["progress"] = stmOtaProgress;
        doc["message"] = stmOtaMessage;
        String out;
        serializeJson(doc, out);
        server.send(200, "application/json", out);
    });

    server.onNotFound(handleStaticRequest);
}

// ----------------------------------------------------------------------------
// WiFi lifecycle
// ----------------------------------------------------------------------------
static void startMDNS() {
    MDNS.end();
    if (MDNS.begin(deviceName)) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("eviqtech", "tcp", 80);
        String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString()
                                                        : WiFi.softAPIP().toString();
        String txt = "ip=" + ipStr + ",mfr=EVIQTech,id=" + WiFi.macAddress() +
                     ",apip=" + WiFi.softAPIP().toString();
        MDNS.addServiceTxt("eviqtech", "tcp", "meta", txt.c_str());
        // Serial.printf("mDNS started: http://%s.local\n", deviceName);
    } else {
        Serial.println("Error setting up MDNS responder!");
    }
}

static void bringUpAP() {
    WiFi.softAPConfig(AP_IP, AP_IP, AP_SUBNET);
    if (isAPPassSet) {
        WiFi.softAP(apName().c_str(), apPassword);
    } else {
        WiFi.softAP(apName().c_str());
    }
}

static void startAPMode() {
    WiFi.mode(WIFI_AP);
    bringUpAP();
    wiFiMode = 'A';
    startMDNS();
}

static void beginSTA() {
    if (!isWiFiSet) return;
    cloudLogf("[WiFi] connecting to '%s'...", ssidBuffer);
    WiFi.mode(WIFI_AP_STA);
    bringUpAP();
    WiFi.begin(ssidBuffer, wifiPassBuffer);
    wiFiMode = 'C';
    staConnectStart = millis();
    lastStaPoll = millis();
}

static void onStaConnected() {
    wiFiMode = 'W';
    // Keep the SoftAP up IN PARALLEL with the station link.
    WiFi.mode(WIFI_AP_STA);
    cloudLogf("[WiFi] connected: ip=%s rssi=%d (AP kept on)", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    startMDNS();
    startNtpSync();
    timeSynced = true;
}

void initWiFiPortal() {
    WiFi.persistent(false);

    server.collectHeaders("Cookie");   // capture the Cookie header for session auth
    registerRoutes();

    startAPMode();
    if (isWiFiSet) beginSTA();

    server.begin();
}

bool portalWiFiConnected() {
    return wiFiMode == 'W' && WiFi.status() == WL_CONNECTED;
}

void serviceWiFiPortal() {
    server.handleClient();
    MDNS.update();

    unsigned long now = millis();

    if (otaRebootPending && (now - otaRebootAt > 800)) {
        delay(50);
        ESP.restart();
    }

    if (isWifiChanged) {
        isWifiChanged = false;
        if (isWiFiSet) beginSTA();
        else startAPMode();
        return;
    }

    if (wiFiMode == 'C') {
        if (now - lastStaPoll >= STA_POLL_INTERVAL_MS) {
            lastStaPoll = now;
            if (WiFi.status() == WL_CONNECTED) {
                onStaConnected();
            } else if (now - staConnectStart > STA_CONNECT_TIMEOUT_MS) {
                cloudLog("[WiFi] STA connect timed out -> AP fallback");
                startAPMode();
            }
        }
        return;
    }

    if (now - lastServiceTick < WIFI_CHECK_INTERVAL_MS) return;
    lastServiceTick = now;

    if (wiFiMode == 'W') {
        if (WiFi.status() != WL_CONNECTED) {
            cloudLog("[WiFi] link lost -> reconnecting");
            beginSTA();
        }
    } else if (wiFiMode == 'A') {
        static unsigned long lastApRetry = 0;
        if (isWiFiSet && (now - lastApRetry > AP_RETRY_INTERVAL_MS)) {
            lastApRetry = now;
            beginSTA();
        }
    }
}
