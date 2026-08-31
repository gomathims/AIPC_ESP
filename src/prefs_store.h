#ifndef PREFS_STORE_H
#define PREFS_STORE_H

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// Minimal drop-in replacement for the ESP32 "Preferences" (NVS) API, backed by
// LittleFS on the ESP8266 (which has no NVS). Each namespace is a small JSON
// file "/nvs_<ns>.json". Only the handful of methods the ported code needs are
// implemented: begin/end, getString/putString, getBool/putBool, remove.
//
// Writes are committed immediately (put* rewrites the file), matching how the
// original code used NVS. Not designed for high-frequency writes.
class Preferences {
  public:
    bool begin(const char *ns, bool readOnly = false) {
        _path = String("/nvs_") + ns + ".json";
        _readOnly = readOnly;
        _doc.clear();
        LittleFS.begin();   // idempotent; safe if already mounted
        File f = LittleFS.open(_path, "r");
        if (f) {
            deserializeJson(_doc, f);   // ignore errors -> empty doc
            f.close();
        }
        return true;
    }

    void end() { _doc.clear(); }

    String getString(const char *key, const String &def = String()) {
        if (_doc[key].is<const char *>()) return String((const char *)_doc[key]);
        return def;
    }

    bool getBool(const char *key, bool def = false) {
        if (_doc[key].is<bool>()) return _doc[key].as<bool>();
        return def;
    }

    void putString(const char *key, const String &val) {
        _doc[key] = val;
        flush();
    }

    void putBool(const char *key, bool val) {
        _doc[key] = val;
        flush();
    }

    void remove(const char *key) {
        _doc.remove(key);
        flush();
    }

  private:
    void flush() {
        if (_readOnly) return;
        File f = LittleFS.open(_path, "w");
        if (!f) return;
        serializeJson(_doc, f);
        f.close();
    }

    String _path;
    bool _readOnly = false;
    JsonDocument _doc;
};

#endif // PREFS_STORE_H
