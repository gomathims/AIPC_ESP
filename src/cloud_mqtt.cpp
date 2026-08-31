#include "cloud_mqtt.h"

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdarg.h>

#include "prefs_store.h"             // LittleFS-backed Preferences shim
#include "preference_management.h"   // deviceName
#include "meter_state.h"
#include "wifi_portal.h"             // portalWiFiConnected()
#include "letsencrypt_ca.h"          // LETSENCRYPT_ISRG_ROOT_X1
#include "ota_update.h"              // processOTASharedAttributes / handleStartOTARpc

// ---- Cloud endpoints (overridable via build_flags in platformio.ini) ----
#ifndef THINGSBOARD_SERVER_URL
#define THINGSBOARD_SERVER_URL "https://iot.eviqtech.com"
#endif
#ifndef THINGSBOARD_REGISTER_URI
#define THINGSBOARD_REGISTER_URI "/api/device/registerDevice"
#endif
#ifndef THINGSBOARD_MQTT
#define THINGSBOARD_MQTT "iot.eviqtech.com"
#endif
#ifndef CLOUD_DEVICE_TYPE
#define CLOUD_DEVICE_TYPE "SuperSmart"
#endif

#define MQTT_PORT 1883
#define TELEMETRY_TOPIC "v1/devices/me/telemetry"
#define ATTRIBUTES_TOPIC "v1/devices/me/attributes"
#define TELEMETRY_INTERVAL_MS 10000UL
#define MQTT_BACKOFF_MIN 1000UL
#define MQTT_BACKOFF_MAX 60000UL

// ---- Cloud log streaming (ThingsBoard "log" telemetry key) -----------------
#define LOG_QUEUE_MAX     16
#define LOG_MSG_MAX       200
#define LOG_FLUSH_MIN_MS  200UL
#define LOG_TS_VALID_MIN  1700000000UL

#if defined(FIRMWARE_MAJOR) && defined(FIRMWARE_MINOR) && defined(FIRMWARE_PATCH)
#define CL_FW_VER (String(FIRMWARE_MAJOR) + "." + String(FIRMWARE_MINOR) + "." + String(FIRMWARE_PATCH))
#else
#define CL_FW_VER String("1.0.0")
#endif

static Preferences cloudPrefs;
static const char *CL_NS = "cloud";

static char cloudEmail[121] = {0};
static char cloudToken[64] = {0};
static bool cloudEnabled = false;

static volatile bool registerPending = false;
static String pendingEmail = "";
static String cloudStatusMsg = "Not connected to cloud.";

static WiFiClient mqttNet;
static PubSubClient mqttClient(mqttNet);

static unsigned long lastTelemetry = 0;
static unsigned long mqttNextRetry = 0;
static unsigned long mqttBackoff = MQTT_BACKOFF_MIN;

// Cloud log ring buffer (single producer: the loop() task).
static String logQueue[LOG_QUEUE_MAX];
static uint8_t logHead = 0;
static uint8_t logCount = 0;
static uint32_t logDropped = 0;
static unsigned long lastLogFlush = 0;
static uint64_t lastLogTs = 0;

// ---- Low-level access + inbound MQTT for the OTA module --------------------
bool cloudIsConnected()        { return mqttClient.connected(); }
const char *cloudAccessToken() { return cloudToken; }
bool cloudPublish(const char *topic, const char *payload) {
    return mqttClient.connected() && mqttClient.publish(topic, payload);
}

static void onMqttMessage(char *topic, byte *payload, unsigned int len) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;

    String t(topic);
    if (t == ATTRIBUTES_TOPIC || t.startsWith("v1/devices/me/attributes/response/")) {
        processOTASharedAttributes(doc);
    } else if (t.startsWith("v1/devices/me/rpc/request/")) {
        const char *method = doc["method"] | "";
        if (strcmp(method, "startOTA") == 0) {
            handleStartOTARpc(t.substring(strlen("v1/devices/me/rpc/request/")));
        }
    }
}

// ----------------------------------------------------------------------------
static void saveCloud() {
    cloudPrefs.begin(CL_NS, false);
    cloudPrefs.putString("email", cloudEmail);
    cloudPrefs.putString("token", cloudToken);
    cloudPrefs.putBool("enabled", cloudEnabled);
    cloudPrefs.end();
}

void initCloud() {
    cloudPrefs.begin(CL_NS, true);
    String e = cloudPrefs.getString("email", "");
    String t = cloudPrefs.getString("token", "");
    cloudEnabled = cloudPrefs.getBool("enabled", false);
    cloudPrefs.end();

    strncpy(cloudEmail, e.c_str(), sizeof(cloudEmail) - 1);
    cloudEmail[sizeof(cloudEmail) - 1] = '\0';
    strncpy(cloudToken, t.c_str(), sizeof(cloudToken) - 1);
    cloudToken[sizeof(cloudToken) - 1] = '\0';

    mqttClient.setServer(THINGSBOARD_MQTT, MQTT_PORT);
    mqttClient.setSocketTimeout(3);
    mqttClient.setKeepAlive(30);
    mqttClient.setBufferSize(1024);
    mqttClient.setCallback(onMqttMessage);

    if (cloudEnabled && strlen(cloudToken) > 0) {
        cloudStatusMsg = "Cloud configured. Connecting when online…";
    }
    cloudLogf("[BOOT] firmware %s starting (cloud %s)",
              CL_FW_VER.c_str(), cloudEnabled ? "enabled" : "not configured");
}

// Blocking HTTPS registration. Runs from serviceCloud() (loop context).
static bool registerDevice(const String &email) {
    if (!portalWiFiConnected()) {
        cloudStatusMsg = "Please connect the device to WiFi first.";
        return false;
    }
    // The registration response is not security-critical (it returns a device
    // token over a POST); to fit the BearSSL handshake in RAM alongside the web
    // server we skip chain validation. Switch to setTrustAnchors() if RAM allows.
    WiFiClientSecure secure;
    secure.setInsecure();
    secure.setBufferSizes(4096, 512);

    HTTPClient http;
    String url = String(THINGSBOARD_SERVER_URL) + THINGSBOARD_REGISTER_URI;
    if (!http.begin(secure, url)) {
        cloudStatusMsg = "There is a problem communicating with Server, Please try after sometime.";
        return false;
    }
    http.setTimeout(8000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Mac_id", WiFi.macAddress());

    JsonDocument body;
    body["email"] = email;
    body["deviceName"] = deviceName;
    body["deviceType"] = CLOUD_DEVICE_TYPE;
    String payload;
    serializeJson(body, payload);

    int code = http.POST(payload);
    String resp = http.getString();
    http.end();
    cloudLogf("[CLOUD] device registration -> HTTP %d", code);

    String mac = WiFi.macAddress();
    if (code == 200 || code == 201 || code == 202) {
        JsonDocument doc;
        if (!deserializeJson(doc, resp) && doc["accessToken"].is<const char *>()) {
            strncpy(cloudToken, doc["accessToken"].as<const char *>(), sizeof(cloudToken) - 1);
            cloudToken[sizeof(cloudToken) - 1] = '\0';
            strncpy(cloudEmail, email.c_str(), sizeof(cloudEmail) - 1);
            cloudEmail[sizeof(cloudEmail) - 1] = '\0';
            cloudEnabled = true;
            saveCloud();
            mqttBackoff = MQTT_BACKOFF_MIN;
            mqttNextRetry = 0;
            if (code == 200) {
                cloudStatusMsg = "Cloud is connected. You will be getting an email, click on the link to "
                                 "activate your account.";
            } else if (code == 201) {
                cloudStatusMsg = "Cloud is connected to your existing account, You will be able to see the new "
                                 "device in your dashboard.";
            } else {
                cloudStatusMsg = "Cloud is reconnected to your existing account, Your device will sync data with cloud.";
            }
            return true;
        }
        cloudStatusMsg = "Error communicating with the server, please try again later";
        return false;
    } else if (code == 420) {
        cloudStatusMsg = "Device already registered with another email, Please contact EVIQtech support, with your device MAC address: " + mac;
    } else if (code == 400) {
        cloudStatusMsg = "Device is not registered with the server, Please contact EVIQtech support, with your device MAC address: " + mac;
    } else if (code == 409) {
        cloudStatusMsg = "Not able to register with this email, Please try with another email, if the problem persists, please contact EVIQtech support, with your device MAC address: " + mac;
    } else {
        cloudStatusMsg = "There is a problem communicating with Server, Please try after sometime, if the problem persists, please contact EVIQtech support, with your device MAC address: " + mac;
    }
    return false;
}

static void publishAttributes() {
    JsonDocument doc;
    doc["deviceName"] = deviceName;
    doc["deviceType"] = CLOUD_DEVICE_TYPE;
    doc["firmwareVersion"] = CL_FW_VER;
    String out;
    serializeJson(doc, out);
    mqttClient.publish(ATTRIBUTES_TOPIC, out.c_str());
}

// ---- Time-series helpers ---------------------------------------------------
static uint64_t cloudEpochMs() {
    time_t now = time(nullptr);
    if (now < (time_t)LOG_TS_VALID_MIN) return 0;
    return (uint64_t)now * 1000ULL;
}

static JsonObject openTsValues(JsonDocument &doc) {
    uint64_t ts = cloudEpochMs();
    if (ts != 0) {
        if (ts <= lastLogTs) ts = lastLogTs + 1;
        lastLogTs = ts;
        doc["ts"] = ts;
        return doc["values"].to<JsonObject>();
    }
    return doc.to<JsonObject>();
}

// Fill the 3-phase V/I/P/E telemetry snapshot into a values object.
static void fillTelemetry(JsonObject v) {
    v["v1"] = round(voltL1 * 10.0) / 10.0;
    v["v2"] = round(voltL2 * 10.0) / 10.0;
    v["v3"] = round(voltL3 * 10.0) / 10.0;
    v["i1"] = round(curL1 * 100.0) / 100.0;
    v["i2"] = round(curL2 * 100.0) / 100.0;
    v["i3"] = round(curL3 * 100.0) / 100.0;
    v["p1"] = round(powL1 * 10.0) / 10.0;
    v["p2"] = round(powL2 * 10.0) / 10.0;
    v["p3"] = round(powL3 * 10.0) / 10.0;
    v["e1"] = round(eneL1 * 1000.0) / 1000.0;
    v["e2"] = round(eneL2 * 1000.0) / 1000.0;
    v["e3"] = round(eneL3 * 1000.0) / 1000.0;
}

static void publishTelemetry() {
    JsonDocument doc;
    fillTelemetry(openTsValues(doc));
    String out;
    serializeJson(doc, out);
    mqttClient.publish(TELEMETRY_TOPIC, out.c_str());
}

static void connectMQTT() {
    if (strlen(cloudToken) == 0) return;
    if (mqttClient.connect(deviceName, cloudToken, NULL)) {
        cloudStatusMsg = "Connected to EVIQtech Cloud.";
        publishAttributes();
        mqttClient.subscribe("v1/devices/me/attributes");
        mqttClient.subscribe("v1/devices/me/attributes/response/+");
        mqttClient.subscribe("v1/devices/me/rpc/request/+");
        requestOTASharedAttributes();
        lastTelemetry = 0;
        mqttBackoff = MQTT_BACKOFF_MIN;
        cloudLogf("[CLOUD] MQTT connected to %s as '%s'", THINGSBOARD_MQTT, deviceName);
    } else {
        cloudStatusMsg = "Connecting to cloud…";
        cloudLogf("[CLOUD] MQTT connect failed (state=%d), retry in %lus",
                  mqttClient.state(), mqttBackoff / 1000UL);
        mqttBackoff = min(mqttBackoff * 2, MQTT_BACKOFF_MAX);
    }
    mqttNextRetry = millis() + mqttBackoff;
}

// ---- Outbound telemetry queue ----------------------------------------------
static void enqueuePayload(const String &json) {
    if (logCount == LOG_QUEUE_MAX) {
        logHead = (logHead + 1) % LOG_QUEUE_MAX;
        logCount--;
        logDropped++;
    }
    uint8_t tail = (logHead + logCount) % LOG_QUEUE_MAX;
    logQueue[tail] = json;
    logCount++;
}

void cloudLog(const String &msg) {
    String line = (msg.length() > LOG_MSG_MAX) ? msg.substring(0, LOG_MSG_MAX) : msg;
    JsonDocument doc;
    openTsValues(doc)["log"] = line;
    String out;
    serializeJson(doc, out);
    enqueuePayload(out);
}

void cloudLogf(const char *fmt, ...) {
    char buf[LOG_MSG_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cloudLog(String(buf));
}

void cloudPublishEvent(uint8_t eventCode, const char *eventName) {
    JsonDocument doc;
    JsonObject v = openTsValues(doc);
    v["event"] = eventCode;
    v["eventMessage"] = eventName;
    fillTelemetry(v);
    String out;
    serializeJson(doc, out);
    enqueuePayload(out);
}

static void flushQueue() {
    if (!mqttClient.connected()) return;
    unsigned long now = millis();
    if (now - lastLogFlush < LOG_FLUSH_MIN_MS) return;

    if (logDropped > 0) {
        JsonDocument doc;
        openTsValues(doc)["log"] = String("[LOG] ") + logDropped + " queued message(s) dropped (queue full)";
        String out;
        serializeJson(doc, out);
        if (mqttClient.publish(TELEMETRY_TOPIC, out.c_str())) { logDropped = 0; lastLogFlush = now; }
        return;
    }
    if (logCount == 0) return;

    if (mqttClient.publish(TELEMETRY_TOPIC, logQueue[logHead].c_str())) {
        logQueue[logHead] = String();
        logHead = (logHead + 1) % LOG_QUEUE_MAX;
        logCount--;
        lastLogFlush = now;
    }
}

void serviceCloud() {
    if (!portalWiFiConnected()) return;

    if (registerPending) {
        registerPending = false;
        registerDevice(pendingEmail);
        pendingEmail = "";
        return;
    }

    if (!cloudEnabled) return;

    if (!mqttClient.connected()) {
        if ((long)(millis() - mqttNextRetry) >= 0) connectMQTT();
        return;
    }

    mqttClient.loop();
    flushQueue();
    if (millis() - lastTelemetry > TELEMETRY_INTERVAL_MS) {
        lastTelemetry = millis();
        publishTelemetry();
    }
}

void cloudRequestConnect(const String &email) {
    pendingEmail = email;
    registerPending = true;
    cloudStatusMsg = "Registering with cloud…";
}

void cloudDisconnect() {
    cloudEnabled = false;
    cloudToken[0] = '\0';
    saveCloud();
    if (mqttClient.connected()) mqttClient.disconnect();
    cloudStatusMsg = "Successfully disconnected from the cloud";
    cloudLog("[CLOUD] disconnected from cloud (user request)");
}

String getCloudStatusJson() {
    JsonDocument doc;
    doc["configured"] = cloudEnabled;
    doc["connected"] = mqttClient.connected();
    doc["email"] = cloudEmail;
    doc["message"] = cloudStatusMsg;
    String out;
    serializeJson(doc, out);
    return out;
}
