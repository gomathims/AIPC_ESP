#ifndef WS_EVENTS_H
#define WS_EVENTS_H

#include <Arduino.h>

// WebSocket server (port 81). ALL live values shown on the web pages are pushed
// from here; the pages no longer poll HTTP endpoints.
//
// Clients connect with their login session id in the URL (cookie "eviqsid"):
//   ws://<host>:81/live?sid=<id>   dashboard + log page
//   ws://<host>:81/raw?sid=<id>    raw UART stream (rawevent.html)
// An invalid / expired session gets {"type":"auth","ok":false} and is closed.
//
// "/live" messages (JSON text frames, each has a "type"):
//   readings  V/I/P/E, cumulative, battery, relays, contactors, time   (new STM frame, max 2/s; >= every 2 s)
//   prot      protection limits + live values + status                 (every 2 s, and after a save)
//   wifi      WiFi / AP status                                         (on connect + when it changes)
//   cloud     cloud status                                             (on connect + when it changes)
//   logs      full event list, newest first                            (on connect + after clear)
//   log       one new event {ts, ev, rc}                               (when it is logged)
// A new client first receives a snapshot of wifi, cloud, prot, readings and logs.
//
// "/raw" messages: one per UART burst
//   {"t":<millis>,"len":n,"hex":"53544d...","json":{...}} | {...,"err":"InvalidInput"}

void initWsEvents();                              // start the WebSocket server. Call once from setup().
void serviceWsEvents();                           // ws loop + periodic pushes. Call every loop().
void wsTapUart(const uint8_t *data, size_t len);  // feed bytes read from the UART (raw stream).

void wsPushLogRecord(uint32_t ts, uint8_t ev, uint8_t aux);   // a new event was logged
void wsPushLogs();                                            // log list changed (e.g. cleared)
void wsPushProtection();                                      // protection settings changed

#endif // WS_EVENTS_H
