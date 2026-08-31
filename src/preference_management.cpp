#include "preference_management.h"

#include "prefs_store.h"   // LittleFS-backed Preferences shim (ESP8266 has no NVS)

// All device settings live in a single namespace ("/nvs_dev.json" on LittleFS).
static Preferences prefs;
static const char *PREF_NS = "dev";

char ssidBuffer[33] = {0};
char wifiPassBuffer[65] = {0};
char deviceName[21] = DEV_DEFAULT_NAME;
char devicePin[9] = DEV_DEFAULT_PIN;
char apPassword[65] = {0};

bool isWiFiSet = false;
bool isAPPassSet = false;
volatile bool isWifiChanged = false;

void initPreferences() {
    prefs.begin(PREF_NS, /*readOnly=*/true);

    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    String name = prefs.getString("name", DEV_DEFAULT_NAME);
    String pin = prefs.getString("pin", DEV_DEFAULT_PIN);
    String appw = prefs.getString("appw", "");

    strncpy(ssidBuffer, ssid.c_str(), sizeof(ssidBuffer) - 1);
    ssidBuffer[sizeof(ssidBuffer) - 1] = '\0';
    strncpy(wifiPassBuffer, pass.c_str(), sizeof(wifiPassBuffer) - 1);
    wifiPassBuffer[sizeof(wifiPassBuffer) - 1] = '\0';

    if (name.length() >= 4) {
        strncpy(deviceName, name.c_str(), sizeof(deviceName) - 1);
        deviceName[sizeof(deviceName) - 1] = '\0';
    }
    if (pin.length() == 4) {
        strncpy(devicePin, pin.c_str(), sizeof(devicePin) - 1);
        devicePin[sizeof(devicePin) - 1] = '\0';
    }
    strncpy(apPassword, appw.c_str(), sizeof(apPassword) - 1);
    apPassword[sizeof(apPassword) - 1] = '\0';

    isWiFiSet = (strlen(ssidBuffer) > 0);
    isAPPassSet = (strlen(apPassword) >= 8);

    prefs.end();
}

void saveWifiCreds(const char *ssid, const char *pass) {
    if (ssid == nullptr) ssid = "";
    if (pass == nullptr) pass = "";

    strncpy(ssidBuffer, ssid, sizeof(ssidBuffer) - 1);
    ssidBuffer[sizeof(ssidBuffer) - 1] = '\0';
    strncpy(wifiPassBuffer, pass, sizeof(wifiPassBuffer) - 1);
    wifiPassBuffer[sizeof(wifiPassBuffer) - 1] = '\0';

    prefs.begin(PREF_NS, /*readOnly=*/false);
    prefs.putString("ssid", ssidBuffer);
    prefs.putString("pass", wifiPassBuffer);
    prefs.end();

    isWiFiSet = (strlen(ssidBuffer) > 0);
    isWifiChanged = true;
}

void clearWifiCreds() {
    ssidBuffer[0] = '\0';
    wifiPassBuffer[0] = '\0';

    prefs.begin(PREF_NS, /*readOnly=*/false);
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();

    isWiFiSet = false;
    isWifiChanged = true;
}

bool saveDeviceName(const char *name) {
    if (name == nullptr) return false;
    size_t len = strlen(name);
    if (len < 4 || len > 20) return false;

    strncpy(deviceName, name, sizeof(deviceName) - 1);
    deviceName[sizeof(deviceName) - 1] = '\0';

    prefs.begin(PREF_NS, /*readOnly=*/false);
    prefs.putString("name", deviceName);
    prefs.end();
    return true;
}

bool saveDevicePin(const char *pin) {
    if (pin == nullptr || strlen(pin) != 4) return false;
    for (int i = 0; i < 4; i++) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    strncpy(devicePin, pin, sizeof(devicePin) - 1);
    devicePin[sizeof(devicePin) - 1] = '\0';

    prefs.begin(PREF_NS, /*readOnly=*/false);
    prefs.putString("pin", devicePin);
    prefs.end();
    return true;
}

bool verifyDevicePin(const char *pin) {
    if (pin == nullptr) return false;
    return strncmp(pin, devicePin, sizeof(devicePin)) == 0 && strlen(pin) == strlen(devicePin);
}

bool saveAPPassword(const char *pass) {
    if (pass == nullptr) pass = "";
    size_t len = strlen(pass);
    if (len > 0 && len < 8) return false;   // WPA2 minimum
    if (len > 63) return false;

    strncpy(apPassword, pass, sizeof(apPassword) - 1);
    apPassword[sizeof(apPassword) - 1] = '\0';
    isAPPassSet = (len >= 8);

    prefs.begin(PREF_NS, /*readOnly=*/false);
    prefs.putString("appw", apPassword);
    prefs.end();
    return true;
}
