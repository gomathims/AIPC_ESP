#include "ws_events.h"

#include <WebSocketsServer.h>

#include "wifi_portal.h"
#include "meter_state.h"
#include "cloud_mqtt.h"

#define WS_PORT 81

// Live push cadence.
#define WS_READINGS_PERIOD_MS 3000UL  // readings every 3 s; a relay / alarm change is sent at once
#define WS_WIFI_CHECK_MS    3000UL    // WiFi status: pushed only when it changed
#define WS_CLOUD_CHECK_MS   2000UL    // cloud status: pushed only when it changed
#define WS_AUTH_CHECK_MS   30000UL    // re-validate each client's session

static WebSocketsServer ws(WS_PORT);

// Per-client state: slot in use + the login session it connected with.
struct WsClient {
    bool used;
    uint32_t sid;
};
static WsClient clients[WEBSOCKETS_SERVER_CLIENT_MAX];
static uint8_t liveCount = 0;

static unsigned long lastReadings = 0, lastWifiCheck = 0,
                     lastCloudCheck = 0, lastAuthCheck = 0;
static uint16_t lastRelaySent = 0;   // relayStatus in the last readings push
static uint8_t lastAlarmSent = 0;    // stmAlarm in the last readings push
static bool forceReadings = false;   // set by wsPushReadingsNow()
static String lastWifiJson, lastCloudJson;

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------
static void recount() {
    liveCount = 0;
    for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (clients[i].used) liveCount++;
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
        if (clients[i].used) ws.sendTXT(i, msg);
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
        uint32_t sid = sidFromUrl((const char *)payload);   // e.g. "/live?sid=123456"
        if (!portalSessionValid(sid)) {
            ws.sendTXT(num, "{\"type\":\"auth\",\"ok\":false}");
            ws.disconnect(num);
            return;
        }
        clients[num].used = true;
        clients[num].sid = sid;
        recount();
        sendSnapshot(num);
    } else if (type == WStype_DISCONNECTED) {
        clients[num].used = false;
        recount();
    }
}

void wsPushReadingsNow() { forceReadings = true; }

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

    if (liveCount == 0) return;
    unsigned long now = millis();

    // Live readings: immediately when the relay state or an alarm changes, otherwise every 3 s.
    uint16_t rs = relayStatus;
    uint8_t al = stmAlarm;
    if (forceReadings || rs != lastRelaySent || al != lastAlarmSent ||
        now - lastReadings >= WS_READINGS_PERIOD_MS) {
        forceReadings = false;
        lastReadings = now;
        lastRelaySent = rs;
        lastAlarmSent = al;
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
