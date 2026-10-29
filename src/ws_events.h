#ifndef WS_EVENTS_H
#define WS_EVENTS_H

#include <Arduino.h>

// WebSocket layer (port 81) that streams raw UART traffic to data/rawevent.html.
//
// Every byte the UART reader consumes is handed to wsTapUart(). Bytes are grouped
// into one "event" per burst (a burst ends after WS_UART_IDLE_MS of silence or when
// the buffer fills) and each event is broadcast as a JSON text frame:
//   {"t":<millis>,"len":n,"hex":"53544d...","json":{...}}      parsed OK
//   {"t":<millis>,"len":n,"hex":"53544d...","err":"InvalidInput"} not a JSON object
//
// Clients must present a valid login session cookie (same one the web UI uses).

void initWsEvents();                              // start the WebSocket server. Call once from setup().
void serviceWsEvents();                           // ws loop + flush pending UART burst. Call every loop().
void wsTapUart(const uint8_t *data, size_t len);  // feed bytes read from the UART.

#endif // WS_EVENTS_H
