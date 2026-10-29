#include "ws_events.h"

#include <WebSocketsServer.h>
#include <ArduinoJson.h>

#include "wifi_portal.h"

#define WS_PORT          81
#define WS_UART_BUF_SIZE 256     // max bytes per broadcast event
#define WS_UART_IDLE_MS  10UL    // gap that ends a UART burst (~115 bytes at 115200)

static WebSocketsServer ws(WS_PORT);

static uint8_t uartBuf[WS_UART_BUF_SIZE];
static size_t uartLen = 0;
static unsigned long uartLastByte = 0;

// Handshake hook: reject clients without a live login session cookie.
static bool validateHeader(String name, String value) {
    if (name.equalsIgnoreCase("Cookie")) return portalCookieAuthed(value);
    return true;
}

static void onWsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    if (type == WStype_CONNECTED) {
        ws.sendTXT(num, "{\"info\":\"connected\"}");
    }
}

static void flushUartBurst() {
    if (uartLen == 0) return;

    static const char HEX_CHARS[] = "0123456789abcdef";
    String hex;
    hex.reserve(uartLen * 2);
    for (size_t i = 0; i < uartLen; i++) {
        hex += HEX_CHARS[uartBuf[i] >> 4];
        hex += HEX_CHARS[uartBuf[i] & 0x0F];
    }

    JsonDocument out;
    out["t"] = millis();
    out["len"] = uartLen;
    out["hex"] = hex;

    JsonDocument parsed;
    DeserializationError err = deserializeJson(parsed, (const char *)uartBuf, uartLen);
    if (err) {
        out["err"] = err.c_str();
    } else if (!parsed.is<JsonObject>()) {
        out["err"] = "Not a JSON object";
    } else {
        out["json"] = parsed.as<JsonObjectConst>();
    }

    String msg;
    serializeJson(out, msg);
    ws.broadcastTXT(msg);
    uartLen = 0;
}

void wsTapUart(const uint8_t *data, size_t len) {
    if (ws.connectedClients() == 0) return;   // nobody watching; skip the work
    for (size_t i = 0; i < len; i++) {
        if (uartLen >= WS_UART_BUF_SIZE) flushUartBurst();
        uartBuf[uartLen++] = data[i];
    }
    uartLastByte = millis();
}

void initWsEvents() {
    const char *mandatory[] = {"Cookie"};
    ws.onValidateHttpHeader(validateHeader, mandatory, 1);
    ws.onEvent(onWsEvent);
    ws.begin();
}

void serviceWsEvents() {
    ws.loop();
    if (uartLen && (millis() - uartLastByte >= WS_UART_IDLE_MS)) flushUartBurst();
}
