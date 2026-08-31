#ifndef PREFERENCE_MANAGEMENT_H
#define PREFERENCE_MANAGEMENT_H

#include <Arduino.h>

// ---- Persisted settings (LittleFS-backed) shared with the WiFi web portal ----

#define DEV_DEFAULT_NAME "AIPC"   // also used for AP name ("<name>_AP") and mDNS host
#define DEV_DEFAULT_PIN  "1234"      // factory default unlock PIN

// Backing buffers (defined in preference_management.cpp).
extern char ssidBuffer[33];        // WiFi SSID (max 32 + null)
extern char wifiPassBuffer[65];    // WiFi password (max 64 + null)
extern char deviceName[21];        // user-facing device name (4..20 chars)
extern char devicePin[9];          // 4-digit unlock PIN
extern char apPassword[65];        // Access Point password ("" = open AP)

extern bool isWiFiSet;             // true when a WiFi SSID has been provisioned
extern bool isAPPassSet;           // true when a (>=8 char) AP password is set
extern volatile bool isWifiChanged; // set when creds change, to trigger a reconnect

// Load all persisted settings into the buffers above. Call once at boot.
void initPreferences();

// Persist WiFi credentials and mark WiFi as provisioned (or cleared if ssid empty).
void saveWifiCreds(const char *ssid, const char *pass);

// Clear stored WiFi credentials (returns the device to AP-only mode on reconnect).
void clearWifiCreds();

// Persist the device name. Returns false if the name is invalid (kept unchanged).
bool saveDeviceName(const char *name);

// Persist a new 4-digit PIN. Returns false if invalid (must be 4 digits).
bool saveDevicePin(const char *pin);

// Compare a candidate PIN against the stored one.
bool verifyDevicePin(const char *pin);

// Persist the AP password ("" clears it -> open AP; otherwise must be >=8 chars).
bool saveAPPassword(const char *pass);

#endif
