#include "tft_display.h"

#include <Arduino.h>
#include <SPI.h>
#include <ESP8266WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <time.h>

#include "meter_state.h"
#include "cumulative_energy.h"
#include "time_util.h"

// ---- Pins ------------------------------------------------------------------
#define TFT_CS   15     // CS0
#define TFT_DC   10     // needs flash mode DIO
#ifndef TFT_RST_PIN
#define TFT_RST_PIN -1  // GPIO9 not driven (flash HOLD line) - see tft_display.h
#endif

#define TFT_SPI_HZ       20000000UL   // 20 MHz: safe for jumper wires
#define TFT_REFRESH_MS   500UL
#define METER_STALE_MS   5000UL       // no STM frame for 5 s -> "NO DATA"

static Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST_PIN);

// ---- Layout (landscape 320 x 240) ------------------------------------------
#define SCR_W     320
#define HDR_H     26
#define COLHDR_Y  32
#define ROW_Y0    50
#define ROW_H     42
#define FTR_Y     220

#define COL_BG      ILI9341_BLACK
#define COL_GRID    0x4208           // dark grey
#define COL_TEXT    ILI9341_WHITE
#define COL_DIM     0xAD55           // light grey
#define COL_HDR_BG  0x1A6B           // dark teal
#define COL_R       ILI9341_RED
#define COL_Y       ILI9341_YELLOW
#define COL_B       0x3D9F           // light blue (pure blue is hard to read on black)
#define COL_INV     ILI9341_GREEN

// Value columns: x start and width (text size 2 = 12 px per char).
static const int16_t COL_X[4] = { 48, 114, 180, 248 };
static const int16_t COL_W[4] = { 66,  66,  68,  72 };
static const char   *COL_NAME[4] = { "Volt V", "Curr A", "Power W", "Cum kWh" };

// Row order matches the web page: Inverter first, then R, Y, B.
struct RowDef { const char *name; uint16_t color; int ch; };   // ch: 0..2 = L1..L3, 3 = L4
static const RowDef ROWS[4] = {
    { "INV", COL_INV, 3 },
    { "R",   COL_R,   0 },
    { "Y",   COL_Y,   1 },
    { "B",   COL_B,   2 },
};

// Last drawn strings, so only changed cells are repainted (no flicker).
static char shown[4][4][10];
static char shownTime[32], shownFooter[48], shownState[12];
static unsigned long lastDrawMs = 0;
static bool displayOk = false;

static void readChannel(int ch, float &v, float &i, float &p) {
    switch (ch) {
        case 0:  v = voltL1; i = curL1; p = powL1; break;
        case 1:  v = voltL2; i = curL2; p = powL2; break;
        case 2:  v = voltL3; i = curL3; p = powL3; break;
        default: v = voltL4; i = curL4; p = powL4; break;
    }
}

// Right-align `txt` inside [x, x+w) at text size 2, painting the background.
static void drawCell(int16_t x, int16_t y, int16_t w, const char *txt, uint16_t fg) {
    tft.fillRect(x, y, w, 16, COL_BG);
    int16_t tw = (int16_t)strlen(txt) * 12;
    tft.setTextSize(2);
    tft.setTextColor(fg, COL_BG);
    tft.setCursor(x + w - tw - 4, y);
    tft.print(txt);
}

// Format v with `dec` decimals; drop decimals if it would not fit in `maxChars`.
static void fmtFit(char *out, size_t n, float v, int dec, int maxChars) {
    for (int d = dec; d >= 0; d--) {
        snprintf(out, n, "%.*f", d, v);
        if ((int)strlen(out) <= maxChars) return;
    }
    snprintf(out, n, "----");
}

static void drawStaticLayout() {
    tft.fillScreen(COL_BG);

    // Header bar
    tft.fillRect(0, 0, SCR_W, HDR_H, COL_HDR_BG);
    tft.setTextSize(2);
    tft.setTextColor(COL_TEXT, COL_HDR_BG);
    tft.setCursor(6, 5);
    tft.print("AIPC Monitor");

    // Column headers
    tft.setTextSize(1);
    tft.setTextColor(COL_DIM, COL_BG);
    for (int c = 0; c < 4; c++) {
        int16_t tw = (int16_t)strlen(COL_NAME[c]) * 6;
        tft.setCursor(COL_X[c] + COL_W[c] - tw - 4, COLHDR_Y + 4);
        tft.print(COL_NAME[c]);
    }
    tft.drawFastHLine(0, ROW_Y0 - 4, SCR_W, COL_GRID);

    // Phase labels + row separators (thicker line after the Inverter row, like the web page)
    for (int r = 0; r < 4; r++) {
        int16_t y = ROW_Y0 + r * ROW_H;
        tft.fillRect(4, y + 4, 4, ROW_H - 12, ROWS[r].color);   // colour tag
        tft.setTextSize(2);
        tft.setTextColor(ROWS[r].color, COL_BG);
        tft.setCursor(12, y + 10);
        tft.print(ROWS[r].name);
        int16_t ly = y + ROW_H - 4;
        tft.drawFastHLine(0, ly, SCR_W, COL_GRID);
        if (r == 0) tft.drawFastHLine(0, ly + 1, SCR_W, COL_DIM);
    }

    memset(shown, 0, sizeof(shown));
    shownTime[0] = shownFooter[0] = shownState[0] = '\0';
}

// Power-on check: solid colour fills prove SPI, DC and backlight all work.
static void colourSelfTest() {
    const uint16_t cols[] = { ILI9341_RED, ILI9341_GREEN, ILI9341_BLUE, ILI9341_WHITE };
    for (uint16_t c : cols) { tft.fillScreen(c); delay(250); yield(); }

    tft.fillScreen(COL_BG);
    tft.setTextSize(2);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setCursor(10, 10);
    tft.print("TFT self-test");
    // Readback over MISO: a working panel returns non-0x00 / non-0xFF values.
    uint8_t pm   = tft.readcommand8(ILI9341_RDMODE);
    uint8_t diag = tft.readcommand8(ILI9341_RDSELFDIAG);
    tft.setTextSize(1);
    tft.setCursor(10, 40);
    tft.printf("Power mode : 0x%02X", pm);
    tft.setCursor(10, 54);
    tft.printf("Self diag  : 0x%02X", diag);
    tft.setCursor(10, 68);
    tft.print((pm == 0x00 || pm == 0xFF) ? "MISO readback: FAIL (check MISO)" : "MISO readback: OK");
    delay(1500);
}

void initDisplay() {
    SPI.begin();
    tft.begin(TFT_SPI_HZ);    // with RST = -1 the library sends a software reset
    tft.setRotation(1);       // landscape, 320 x 240
    colourSelfTest();
    drawStaticLayout();
    displayOk = true;
    lastDrawMs = 0;
}

static void drawHeaderTime() {
    char buf[32] = "--:--:--";
    time_t e = curTime;
    if (e >= (time_t)TS_VALID_MIN) {
        struct tm t;
        gmtime_r(&e, &t);                         // STM RTC already holds local (IST) time
        strftime(buf, sizeof(buf), "%Y-%m-%d / %H:%M:%S", &t);
    }
    if (strcmp(buf, shownTime) == 0) return;
    strlcpy(shownTime, buf, sizeof(shownTime));
    int16_t tw = (int16_t)strlen(buf) * 6;
    tft.fillRect(170, 0, SCR_W - 170, HDR_H, COL_HDR_BG);
    tft.setTextSize(1);
    tft.setTextColor(COL_TEXT, COL_HDR_BG);
    tft.setCursor(SCR_W - tw - 6, 9);
    tft.print(buf);
}

static void drawFooter(bool live) {
    char buf[48];
    if (WiFi.status() == WL_CONNECTED)
        snprintf(buf, sizeof(buf), "Bat %.2f/3.3V  IP %s", bateryVoltag, WiFi.localIP().toString().c_str());
    else
        snprintf(buf, sizeof(buf), "Bat %.2f/3.3V  WiFi: offline", bateryVoltag);
    if (strcmp(buf, shownFooter) != 0) {
        strlcpy(shownFooter, buf, sizeof(shownFooter));
        tft.fillRect(0, FTR_Y, 250, 240 - FTR_Y, COL_BG);
        tft.setTextSize(1);
        tft.setTextColor(COL_DIM, COL_BG);
        tft.setCursor(6, FTR_Y + 6);
        tft.print(buf);
    }

    const char *st = live ? "LIVE" : "NO DATA";
    if (strcmp(st, shownState) != 0) {
        strlcpy(shownState, st, sizeof(shownState));
        uint16_t c = live ? ILI9341_GREEN : ILI9341_RED;
        tft.fillRect(250, FTR_Y, SCR_W - 250, 240 - FTR_Y, COL_BG);
        tft.setTextSize(1);
        tft.setTextColor(c, COL_BG);
        int16_t tw = (int16_t)strlen(st) * 6;
        tft.setCursor(SCR_W - tw - 6, FTR_Y + 6);
        tft.print(st);
    }
}

void serviceDisplay() {
    if (!displayOk) return;
    unsigned long now = millis();
    if (now - lastDrawMs < TFT_REFRESH_MS) return;
    lastDrawMs = now;

    bool live = meterDataValid && (now - meterLastUpdate < METER_STALE_MS);

    for (int r = 0; r < 4; r++) {
        float v, i, p;
        readChannel(ROWS[r].ch, v, i, p);
        char txt[4][10];
        if (live) {
            fmtFit(txt[0], sizeof(txt[0]), v, 1, 5);
            fmtFit(txt[1], sizeof(txt[1]), i, 2, 5);
            fmtFit(txt[2], sizeof(txt[2]), p, 0, 5);
        } else {
            strcpy(txt[0], "--"); strcpy(txt[1], "--"); strcpy(txt[2], "--");
        }
        fmtFit(txt[3], sizeof(txt[3]), cumEnergy[ROWS[r].ch], 2, 6);   // saved value, valid offline too

        int16_t y = ROW_Y0 + r * ROW_H + 10;
        for (int c = 0; c < 4; c++) {
            if (strcmp(txt[c], shown[r][c]) == 0) continue;
            strlcpy(shown[r][c], txt[c], sizeof(shown[r][c]));
            drawCell(COL_X[c], y, COL_W[c], txt[c], COL_TEXT);
            yield();
        }
    }
    drawHeaderTime();
    drawFooter(live);
}
