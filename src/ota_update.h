#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <Arduino.h>
#include <ArduinoJson.h>

// Cloud (ThingsBoard) firmware-over-the-air for the ESP8266 meter.
//
// Flow:
//   1. A firmware package is assigned to the device in ThingsBoard; the server
//      pushes fw_* shared attributes over MQTT.
//   2. The device parses them; if the assigned version is NEWER than the compiled
//      FIRMWARE_MAJOR.MINOR.PATCH it AUTO-STARTS the update (no button).
//   3. performOTAUpdate() streams the binary over HTTPS into the OTA area, verifies
//      it (MD5 if provided) and reboots.
//
// NOTE: unlike the ESP32 original, the ESP8266 has no A/B partition rollback. A
// bad image simply fails Update.end()/MD5 and is not applied; a genuinely broken
// flashed image requires a serial re-flash.

enum OTAState : uint8_t {
    OTA_IDLE = 0,
    OTA_DOWNLOADING = 1,
    OTA_UPDATING = 2,
    OTA_UPDATED = 3,
    OTA_FAILED = 4
};

struct FirmwareInfo {
    char title[64];
    char version[32];
    uint32_t size;
    char checksum[65];      // MD5=32 hex, SHA256=64 hex
    char checksumAlg[16];   // "MD5", "SHA256", etc.
    bool available;         // true when fw_* received and version is newer
};

extern FirmwareInfo targetFirmware;
extern volatile OTAState otaState;
extern volatile uint8_t otaProgress;    // 0-100
extern char otaErrorMessage[128];

void processOTASharedAttributes(JsonDocument &doc);
void handleStartOTARpc(const String &requestId);
bool performOTAUpdate();
bool isOTAUpdateAvailable();
void publishOTAAttributes();
void requestOTASharedAttributes();
void initOTA();
void confirmFirmwareValid();   // no-op on ESP8266 (no rollback); kept for API parity

#endif // OTA_UPDATE_H
