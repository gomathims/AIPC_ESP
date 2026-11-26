#ifndef WS_EVENTS_H
#define WS_EVENTS_H

#include <Arduino.h>

// WebSocket server (port 81). Pushes the live readings, WiFi status and cloud
// status to the dashboard. The event log and protection settings are NOT sent
// here — the pages fetch those over HTTP (/api/getLogs, /api/getProtection).
//
// Clients connect with their login session id in the URL (cookie "eviqsid"):
//   ws://<host>:81/live?sid=<id>
// An invalid / expired session gets {"type":"auth","ok":false} and is closed.
//
// Messages (JSON text frames, each has a "type"):
//   readings  V/I/P/E, cumulative, battery, relays, contactors, alarms, time
//             (at once when relayStatus or an alarm changes, else every 3 s)
//   wifi      WiFi / AP status   (on connect + when it changes)
//   cloud     cloud status       (on connect + when it changes)
// A new client first receives a snapshot of wifi, cloud and readings.

void initWsEvents();        // start the WebSocket server. Call once from setup().
void serviceWsEvents();     // ws loop + periodic pushes. Call every loop().
void wsPushReadingsNow();   // send readings on the next serviceWsEvents() (e.g. after an energy reset).

#endif // WS_EVENTS_H
