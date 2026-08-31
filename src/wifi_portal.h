#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

#include <Arduino.h>

// WiFi (AP fallback + station) web portal for the ESP8266 3-phase meter.
//
// Behaviour:
//  - On boot the device brings up a SoftAP "<deviceName>_AP" at 192.168.4.1 so it
//    is always reachable locally.
//  - If WiFi credentials are provisioned it connects to that network in the
//    background (non-blocking). The SoftAP stays up IN PARALLEL even after the
//    station link is established.
//  - The web UI (served from PROGMEM in web_mappings.h) shows live 3-phase
//    voltage/current/power/energy and lets the user provision WiFi.
//
// Reachable at http://<ip> and http://<deviceName>.local

void initWiFiPortal();       // bring up WiFi + HTTP server. Call once from setup().
void serviceWiFiPortal();    // handleClient + WiFi lifecycle. Call every loop().
bool portalWiFiConnected();  // true when connected in station mode.

#endif // WIFI_PORTAL_H
