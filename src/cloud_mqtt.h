#ifndef CLOUD_MQTT_H
#define CLOUD_MQTT_H

#include <Arduino.h>

// EVIQtech cloud (ThingsBoard MQTT) integration for the ESP8266 3-phase meter.
//
// Flow:
//   1. User enters an email in Settings -> POST /api/connectCloud.
//   2. Device registers over HTTPS at iot.eviqtech.com/api/device/registerDevice
//      and receives an "accessToken".
//   3. The token is the MQTT username; the device connects to iot.eviqtech.com:1883
//      and publishes V/I/P/E telemetry to v1/devices/me/telemetry.
//
// All network work happens only while WiFi (station) is connected.

void initCloud();
void serviceCloud();
void cloudRequestConnect(const String &email);
void cloudDisconnect();
String getCloudStatusJson();

// ---- Low-level MQTT access for the OTA module -------------------------------
bool cloudIsConnected();
const char *cloudAccessToken();
bool cloudPublish(const char *topic, const char *payload);

// Mirror a log line to the cloud (buffered until MQTT is up).
void cloudLog(const String &msg);
void cloudLogf(const char *fmt, ...);

// Publish a structured device event (e.g. restart) as a timestamped point.
void cloudPublishEvent(uint8_t eventCode, const char *eventName);

#endif // CLOUD_MQTT_H
