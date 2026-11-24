#ifndef WS_EVENTS_H
#define WS_EVENTS_H

#include <Arduino.h>

// WebSocket server (port 81). Pushes the live readings, WiFi status and cloud
// status to the web pages. The event log and protection settings are NOT sent
// here — the pages fetch those over HTTP (/api/getLogs, /api/getProtection).
//
// Clients connect with their login session id in the URL (cookie "eviqsid"):
//   ws://<host>:81/live?sid=<id>   dashboard + log page
//   ws://<host>:81/raw?sid=<id>    raw UART stream (rawevent.html)
// An invalid / expired session gets {"type":"auth","ok":false} and is closed.
//
// "/live" messages (JSON text frames, each has a "type"):
//   readings  V/I/P/E, cumulative, battery, relays, contactors, time   (at once when relayStatus changes, else every 3 s)
//   wifi      WiFi / AP status                                         (on connect + when it changes)
//   cloud     cloud status                                             (on connect + when it changes)
// A new client first receives a snapshot of wifi, cloud and readings.
//
// "/raw" messages: one per UART burst
//   {"t":<millis>,"len":n,"hex":"53544d...","json":{...}} | {...,"err":"InvalidInput"}

void initWsEvents();                              // start the WebSocket server. Call once from setup().
void serviceWsEvents();                           // ws loop + periodic pushes. Call every loop().
void wsTapUart(const uint8_t *data, size_t len);  // feed bytes read from the UART (raw stream).

#endif // WS_EVENTS_H
