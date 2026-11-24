#include "ws_events.h"

#include <WebSocketsServer.h>
#include <ArduinoJson.h>

#include "wifi_portal.h"
#include "meter_state.h"
#include "cloud_mqtt.h"

#define WS_PORT          81
#define WS_UART_BUF_SIZE 256     // max bytes per raw broadcast event
#define WS_UART_IDLE_MS  10UL    // gap that ends a UART burst (~115 bytes at 115200)

// Live push cadence.
#define WS_READINGS_PERIOD_MS 3000UL  // readings every 3 s; a relay change is sent at once
#define WS_WIFI_CHECK_MS    3000UL    // WiFi status: pushed only when it changed
#define WS_CLOUD_CHECK_MS   2000UL    // cloud status: pushed only when it changed
#define WS_AUTH_CHECK_MS   30000UL    // re-validate each client's session

static WebSocketsServer ws(WS_PORT);

// Per-client state. "/raw" clients get the raw UART stream (rawevent.html);
// every other URL ("/live") gets the live dashboard data.
struct WsClient {
    bool used;
    bool raw;
    uint32_t sid;
};
static WsClient clients[WEBSOCKETS_SERVER_CLIENT_MAX];
static uint8_t liveCount = 0, rawCount = 0;

static uint8_t uartBuf[WS_UART_BUF_SIZE];
static size_t uartLen = 0;
static unsigned long uartLastByte = 0;

static unsigned long lastReadings = 0, lastWifiCheck = 0,
                     lastCloudCheck = 0, lastAuthCheck = 0;
static uint16_t lastRelaySent = 0;   // relayStatus in the last readings push
static String lastWifiJson, lastCloudJson;

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------
static void recount() {
    liveCount = rawCount = 0;
    for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (!clients[i].used) continue;
        if (clients[i].raw) rawCount++; else liveCount++;
    }
}

// Prefix a JSON object string with "type":"<t>"  ->  {"type":"t", ...}
static String typed(const char *t, const String &obj) {
    String s;
    s.reserve(obj.length() + 16);
    s += "{\"type\":\"";
    s += t;
    s += '"';
    if (obj.length() > 2) { s += ','; s += obj.substring(1); }
    else s += '}';
    return s;
}

static void sendLive(String &msg) {
    for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (clients[i].used && !clients[i].raw) ws.sendTXT(i, msg);
    }
}

static void sendRaw(String &msg) {
    for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (clients[i].used && clients[i].raw) ws.sendTXT(i, msg);
    }
}

static uint32_t sidFromUrl(const char *url) {
    const char *p = strstr(url, "sid=");
    return p ? (uint32_t)strtoul(p + 4, nullptr, 10) : 0;
}

// Full snapshot for a client that just connected.
static void sendSnapshot(uint8_t num) {
    String m;
    m = typed("wifi", portalWiFiStatusJson());     ws.sendTXT(num, m);
    m = typed("cloud", getCloudStatusJson());      ws.sendTXT(num, m);
    m = typed("readings", portalReadingsJson());   ws.sendTXT(num, m);
}

static void onWsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    if (num >= WEBSOCKETS_SERVER_CLIENT_MAX) return;

    if (type == WStype_CONNECTED) {
        String url((const char *)payload);   // e.g. "/live?sid=123456"
        uint32_t sid = sidFromUrl(url.c_str());
        if (!portalSessionValid(sid)) {
            ws.sendTXT(num, "{\"type\":\"auth\",\"ok\":false}");
            ws.disconnect(num);
            return;
        }
        clients[num].used = true;
        clients[num].raw = url.startsWith("/raw");
        clients[num].sid = sid;
        recount();
        if (clients[num].raw) ws.sendTXT(num, "{\"info\":\"connected\"}");
        else sendSnapshot(num);
    } else if (type == WStype_DISCONNECTED) {
        clients[num].used = false;
        recount();
    }
}

// ----------------------------------------------------------------------------
// raw UART stream (rawevent.html)
// ----------------------------------------------------------------------------
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
    sendRaw(msg);
    uartLen = 0;
}

void wsTapUart(const uint8_t *data, size_t len) {
    if (rawCount == 0) return;   // nobody watching the raw stream; skip the work
    for (size_t i = 0; i < len; i++) {
        if (uartLen >= WS_UART_BUF_SIZE) flushUartBurst();
        uartBuf[uartLen++] = data[i];
    }
    uartLastByte = millis();
}

// ----------------------------------------------------------------------------
void initWsEvents() {
    ws.onEvent(onWsEvent);
    ws.begin();
    // Ping every 15 s; drop a client that misses 2 pongs (3 s each). Stops a dead
    // phone/browser connection from holding a slot or blocking sends.
    ws.enableHeartbeat(15000, 3000, 2);
}

void serviceWsEvents() {
    ws.loop();
    if (uartLen && (millis() - uartLastByte >= WS_UART_IDLE_MS)) flushUartBurst();

    if (liveCount == 0) return;
    unsigned long now = millis();

    // Live readings: immediately when the relay state changes, otherwise once every 3 s.
    uint16_t rs = relayStatus;
    if (rs != lastRelaySent || now - lastReadings >= WS_READINGS_PERIOD_MS) {
        lastReadings = now;
        lastRelaySent = rs;
        String m = typed("readings", portalReadingsJson());
        sendLive(m);
    }

    if (now - lastWifiCheck >= WS_WIFI_CHECK_MS) {
        lastWifiCheck = now;
        String s = portalWiFiStatusJson();
        if (s != lastWifiJson) {
            lastWifiJson = s;
            String m = typed("wifi", s);
            sendLive(m);
        }
    }

    if (now - lastCloudCheck >= WS_CLOUD_CHECK_MS) {
        lastCloudCheck = now;
        String s = getCloudStatusJson();
        if (s != lastCloudJson) {
            lastCloudJson = s;
            String m = typed("cloud", s);
            sendLive(m);
        }
    }

    // Sessions expire / get logged out: close those sockets so the page goes to login.
    if (now - lastAuthCheck >= WS_AUTH_CHECK_MS) {
        lastAuthCheck = now;
        for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
            if (clients[i].used && !portalSessionValid(clients[i].sid)) {
                ws.sendTXT(i, "{\"type\":\"auth\",\"ok\":false}");
                ws.disconnect(i);
            }
        }
    }
}
