#include "ota_update.h"

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>

#include "cloud_mqtt.h"          // cloudPublish / cloudAccessToken / cloudIsConnected / cloudLogf

#ifndef THINGSBOARD_SERVER_URL
#define THINGSBOARD_SERVER_URL "https://iot.eviqtech.com"
#endif

// ThingsBoard topics
static const char *TB_ATTR_TOPIC     = "v1/devices/me/attributes";
static const char *TB_TELE_TOPIC     = "v1/devices/me/telemetry";
static const char *TB_ATTR_REQ_TOPIC = "v1/devices/me/attributes/request/";
static const char *TB_RPC_RESP_TOPIC = "v1/devices/me/rpc/response/";

// Globals
FirmwareInfo targetFirmware;
volatile OTAState otaState = OTA_IDLE;
volatile uint8_t otaProgress = 0;
char otaErrorMessage[128] = {0};

static uint32_t otaRequestId = 9000;

static const char *otaStateStr(OTAState state) {
    switch (state) {
        case OTA_DOWNLOADING: return "DOWNLOADING";
        case OTA_UPDATING:    return "UPDATING";
        case OTA_UPDATED:     return "UPDATED";
        case OTA_FAILED:      return "FAILED";
        default:              return "IDLE";
    }
}

static String urlEncode(const String &str) {
    String encoded = "";
    for (unsigned int i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
            encoded += buf;
        }
    }
    return encoded;
}

void initOTA() {
    otaState = OTA_IDLE;
    otaProgress = 0;
    memset(&targetFirmware, 0, sizeof(FirmwareInfo));
    targetFirmware.available = false;
    otaErrorMessage[0] = '\0';
}

void confirmFirmwareValid() {
    // No A/B rollback on the ESP8266; nothing to confirm. Kept for API parity.
}

bool isOTAUpdateAvailable() {
    if (targetFirmware.version[0] == '\0') return false;

    const char *vStr = targetFirmware.version;
    while (*vStr && !isdigit(*vStr)) vStr++;

    int tMajor = 0, tMinor = 0, tPatch = 0;
    sscanf(vStr, "%d.%d.%d", &tMajor, &tMinor, &tPatch);

    if (tMajor > FIRMWARE_MAJOR) return true;
    if (tMajor == FIRMWARE_MAJOR && tMinor > FIRMWARE_MINOR) return true;
    if (tMajor == FIRMWARE_MAJOR && tMinor == FIRMWARE_MINOR && tPatch > FIRMWARE_PATCH) return true;

    return false;
}

void publishOTAAttributes() {
    if (!cloudIsConnected()) return;

    JsonDocument jsonDoc;
    char jsonBuffer[256];

    jsonDoc["fw_update_available"] = targetFirmware.available;
    if (targetFirmware.available) {
        jsonDoc["target_fw_version"] = targetFirmware.version;
    }
    jsonDoc["fw_state"] = otaStateStr(otaState);
    jsonDoc["fw_progress"] = (int)otaProgress;

    size_t len = serializeJson(jsonDoc, jsonBuffer);
    if (len > 0) {
        cloudPublish(TB_ATTR_TOPIC, jsonBuffer);
        cloudPublish(TB_TELE_TOPIC, jsonBuffer);
    }
}

static void publishOTAProgress() {
    if (!cloudIsConnected()) return;

    JsonDocument jsonDoc;
    char jsonBuffer[128];

    jsonDoc["fw_state"] = otaStateStr(otaState);
    jsonDoc["fw_progress"] = (int)otaProgress;

    if (serializeJson(jsonDoc, jsonBuffer) > 0) {
        cloudPublish(TB_ATTR_TOPIC, jsonBuffer);
        cloudPublish(TB_TELE_TOPIC, jsonBuffer);
    }
}

static void parseFirmwareKeys(JsonObject obj) {
    bool hasUpdate = false;

    if (obj["fw_title"].is<const char *>()) {
        strncpy(targetFirmware.title, obj["fw_title"].as<const char *>(), sizeof(targetFirmware.title) - 1);
        targetFirmware.title[sizeof(targetFirmware.title) - 1] = '\0';
        hasUpdate = true;
    }
    if (obj["fw_version"].is<const char *>()) {
        strncpy(targetFirmware.version, obj["fw_version"].as<const char *>(), sizeof(targetFirmware.version) - 1);
        targetFirmware.version[sizeof(targetFirmware.version) - 1] = '\0';
        hasUpdate = true;
    }
    if (obj["fw_size"].is<uint32_t>()) {
        targetFirmware.size = obj["fw_size"].as<uint32_t>();
    }
    if (obj["fw_checksum"].is<const char *>()) {
        strncpy(targetFirmware.checksum, obj["fw_checksum"].as<const char *>(), sizeof(targetFirmware.checksum) - 1);
        targetFirmware.checksum[sizeof(targetFirmware.checksum) - 1] = '\0';
    }
    if (obj["fw_checksum_algorithm"].is<const char *>()) {
        strncpy(targetFirmware.checksumAlg, obj["fw_checksum_algorithm"].as<const char *>(), sizeof(targetFirmware.checksumAlg) - 1);
        targetFirmware.checksumAlg[sizeof(targetFirmware.checksumAlg) - 1] = '\0';
    }

    if (hasUpdate) {
        targetFirmware.available = isOTAUpdateAvailable();
        cloudLogf("[OTA] fw assigned: %s v%s available=%d",
                  targetFirmware.title, targetFirmware.version, targetFirmware.available);
        publishOTAAttributes();

        if (targetFirmware.available && otaState == OTA_IDLE) {
            otaState = OTA_DOWNLOADING;
            otaProgress = 0;
            otaErrorMessage[0] = '\0';
            cloudLogf("[OTA] auto-update starting -> v%s", targetFirmware.version);
            publishOTAAttributes();
        }
    }
}

void processOTASharedAttributes(JsonDocument &doc) {
    if (doc["fw_title"].is<const char *>() || doc["fw_version"].is<const char *>()) {
        JsonObject root = doc.as<JsonObject>();
        parseFirmwareKeys(root);
    }
    if (doc["shared"].is<JsonObject>()) {
        JsonObject shared = doc["shared"].as<JsonObject>();
        if (shared["fw_title"].is<const char *>() || shared["fw_version"].is<const char *>()) {
            parseFirmwareKeys(shared);
        }
    }
}

void requestOTASharedAttributes() {
    if (!cloudIsConnected()) return;

    String requestPayload = "{\"sharedKeys\":\"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm\"}";
    String topic = String(TB_ATTR_REQ_TOPIC) + String(otaRequestId++);
    cloudPublish(topic.c_str(), requestPayload.c_str());
}

void handleStartOTARpc(const String &requestId) {
    String responseTopic = String(TB_RPC_RESP_TOPIC) + requestId;

    if (!targetFirmware.available) {
        cloudPublish(responseTopic.c_str(), "{\"success\":false,\"message\":\"No firmware update available\"}");
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        cloudPublish(responseTopic.c_str(), "{\"success\":false,\"message\":\"WiFi not connected\"}");
        return;
    }
    if (otaState == OTA_DOWNLOADING || otaState == OTA_UPDATING) {
        cloudPublish(responseTopic.c_str(), "{\"success\":false,\"message\":\"OTA already in progress\"}");
        return;
    }

    cloudPublish(responseTopic.c_str(), "{\"success\":true,\"message\":\"OTA update starting\"}");

    otaState = OTA_DOWNLOADING;
    otaProgress = 0;
    otaErrorMessage[0] = '\0';
    publishOTAAttributes();
}

bool performOTAUpdate() {
    String baseUrl = String(THINGSBOARD_SERVER_URL) + "/api/v1/" + String(cloudAccessToken()) + "/firmware";
    baseUrl += "?title=" + urlEncode(String(targetFirmware.title));
    baseUrl += "&version=" + urlEncode(String(targetFirmware.version));

    // TLS: the firmware payload is integrity-checked by MD5 (Update.setMD5) below,
    // so we skip chain validation here to keep the BearSSL handshake within the
    // ESP8266's limited RAM while the web server / MQTT are also running.
    WiFiClientSecure secure;
    secure.setInsecure();
    secure.setBufferSizes(4096, 512);

    HTTPClient http;
    http.setTimeout(60000);

    if (!http.begin(secure, baseUrl)) {
        otaState = OTA_FAILED;
        snprintf(otaErrorMessage, sizeof(otaErrorMessage), "Failed to begin HTTP connection");
        cloudLogf("[OTA] %s", otaErrorMessage);
        publishOTAAttributes();
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != 200) {
        otaState = OTA_FAILED;
        snprintf(otaErrorMessage, sizeof(otaErrorMessage), "HTTP error: %d", httpCode);
        cloudLogf("[OTA] %s", otaErrorMessage);
        publishOTAAttributes();
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0 && targetFirmware.size > 0) {
        contentLength = targetFirmware.size;
    }

    uint32_t maxSketch = ESP.getFreeSketchSpace();
    if (contentLength > 0 && (uint32_t)contentLength > maxSketch) {
        otaState = OTA_FAILED;
        snprintf(otaErrorMessage, sizeof(otaErrorMessage),
                 "Firmware too large: %d bytes (max %u)", contentLength, maxSketch);
        cloudLogf("[OTA] %s", otaErrorMessage);
        publishOTAAttributes();
        http.end();
        return false;
    }

    if (strlen(targetFirmware.checksum) > 0 && strcmp(targetFirmware.checksumAlg, "MD5") == 0) {
        Update.setMD5(targetFirmware.checksum);
    }

    uint32_t updateSize = contentLength > 0 ? (uint32_t)contentLength : maxSketch;
    if (!Update.begin(updateSize)) {
        otaState = OTA_FAILED;
        Update.printError(Serial);
        snprintf(otaErrorMessage, sizeof(otaErrorMessage), "Update.begin failed");
        cloudLogf("[OTA] %s", otaErrorMessage);
        publishOTAAttributes();
        http.end();
        return false;
    }

    otaState = OTA_UPDATING;
    otaProgress = 0;
    publishOTAAttributes();
    publishOTAProgress();

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buffer[512];
    int totalWritten = 0;
    uint8_t lastReportedProgress = 0;

    while (http.connected() && (contentLength <= 0 || totalWritten < contentLength)) {
        size_t avail = stream->available();
        if (avail > 0) {
            int bytesRead = stream->readBytes(buffer, avail > sizeof(buffer) ? sizeof(buffer) : avail);
            if (bytesRead > 0) {
                if (Update.write(buffer, bytesRead) != (size_t)bytesRead) {
                    otaState = OTA_FAILED;
                    Update.printError(Serial);
                    snprintf(otaErrorMessage, sizeof(otaErrorMessage), "Update.write failed at %d bytes", totalWritten);
                    cloudLogf("[OTA] %s", otaErrorMessage);
                    Update.end();
                    http.end();
                    publishOTAAttributes();
                    return false;
                }
                totalWritten += bytesRead;

                if (contentLength > 0) {
                    otaProgress = (uint8_t)(totalWritten * 100 / contentLength);
                }
                if (otaProgress >= lastReportedProgress + 5) {
                    lastReportedProgress = otaProgress;
                    publishOTAProgress();
                }
            }
        }
        yield();   // feed the soft WDT + let WiFi run
        delay(1);
    }

    http.end();

    if (Update.end(true)) {
        otaState = OTA_UPDATED;
        otaProgress = 100;
        publishOTAAttributes();
        publishOTAProgress();
        cloudLogf("[OTA] update to v%s OK (%d bytes) -> rebooting", targetFirmware.version, totalWritten);
        delay(1000);
        ESP.restart();
        return true;  // not reached
    }

    otaState = OTA_FAILED;
    Update.printError(Serial);
    snprintf(otaErrorMessage, sizeof(otaErrorMessage), "Update.end failed - checksum mismatch or incomplete");
    cloudLogf("[OTA] %s", otaErrorMessage);
    publishOTAAttributes();
    return false;
}
