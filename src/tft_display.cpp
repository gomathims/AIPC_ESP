#include "tft_display.h"

#include <Arduino.h>
#include <SPI.h>
#include <ESP8266WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Fonts/FreeSans9pt7b.h>
#include <math.h>
#include <time.h>

#include "meter_state.h"
#include "cumulative_energy.h"
#include "time_util.h"
#include "tft_images.h"     // logo (from data/images/logo.png) + AP icon (ap.png)

// ---- Pins ------------------------------------------------------------------
// SCK = GPIO14, MOSI = GPIO13, MISO = GPIO12 are fixed by the hardware SPI (HSPI).
#define TFT_CS   5      // GPIO5: chip select (no boot-strap restriction)
#define TFT_DC   4      // GPIO4: data/command (no boot-strap restriction)
#ifndef TFT_RST_PIN
#define TFT_RST_PIN -1  // not driven: panel RESET is tied to 3.3 V (10k) or ESP EN/RST
#endif

#define TFT_SPI_HZ       20000000UL   // 20 MHz: safe for jumper wires
#define TFT_REFRESH_MS   500UL
#define METER_STALE_MS   5000UL       // no STM frame for 5 s -> "NO DATA"

static Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST_PIN);

// ---- Layout (landscape 320 x 240) ------------------------------------------
#define SCR_W     320
#define HDR_H     26
#define COLHDR_Y  30
#define ROW_Y0    48
#define ROW_H     43
#define FTR_Y     222
#define LBL_W     80               // line-label column ("L1" or "L3>L1")

#define COL_BG      ILI9341_BLACK
#define COL_GRID    0x4208           // dark grey
#define COL_TEXT    ILI9341_WHITE
#define COL_DIM     0xAD55           // light grey
#define COL_OFF     0x6B4D           // unlit WiFi arc
#define COL_HDR_BG  0x1A6B           // dark teal
#define COL_R       ILI9341_RED
#define COL_Y       ILI9341_YELLOW
#define COL_B       0x3D9F           // light blue (pure blue is hard to read on black)
#define COL_INV     ILI9341_GREEN
#define COL_LOGO    0x4C48           // logo green (73,138,69), same as images/logo.png

// Value columns (FreeSans 9pt digits are ~10 px wide), with a vertical rule at each x.
static const int16_t COL_X[4] = { 80, 136, 190, 248 };
static const int16_t COL_W[4] = { 56,  54,  58,  72 };
static const char   *COL_NAME[4] = { "Volt V", "Curr A", "Power W", "Cum kWh" };

// Lines: index 0..2 = L1..L3, 3 = inverter (same as the meter channels).
static const char    *LINE_NAME[4]  = { "L1", "L2", "L3", "INV" };
static const uint16_t LINE_COLOR[4] = { COL_R, COL_Y, COL_B, COL_INV };

// Row order matches the web page: Inverter first, then L1, L2, L3.
static const int ROW_CH[4] = { 3, 0, 1, 2 };

// Last drawn state, so only changed parts are repainted (no flicker).
static char shown[4][4][10];
static int8_t shownSrc[4];
static int8_t shownNet = -1;
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

// Which line is feeding output `ch` through a changeover relay (relayStatus bits,
// see meter_state.h). Returns -1 when the line's own main relay feeds it (normal).
static int8_t feedSource(int ch, uint16_t rs) {
    if (ch > 2) return -1;
    static const uint8_t MAIN_BIT[3] = { 0, 1, 2 };
    // {aux bit, source line} per output: xyAux = line x feeds output y.
    static const uint8_t AUX[3][2][2] = {
        { { 4, 1 }, { 8, 2 } },   // L1 output: yrAux (L2), brAux (L3)
        { { 3, 0 }, { 6, 2 } },   // L2 output: ryAux (L1), byAux (L3)
        { { 7, 0 }, { 5, 1 } },   // L3 output: rbAux (L1), ybAux (L2)
    };
    if (rs & (1u << MAIN_BIT[ch])) return -1;
    for (int k = 0; k < 2; k++)
        if (rs & (1u << AUX[ch][k][0])) return AUX[ch][k][1];
    if (rs & (1u << 9)) return 3;   // upsMain: inverter feeds all outputs
    return -1;
}

// Right-align a value inside column [x, x+w) of the row starting at rowY (FreeSans 9pt).
static void drawCell(int16_t x, int16_t rowY, int16_t w, const char *txt, uint16_t fg) {
    tft.fillRect(x + 1, rowY + 2, w - 2, ROW_H - 8, COL_BG);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextSize(1);
    int16_t x1, y1; uint16_t tw, th;
    tft.getTextBounds(txt, 0, 0, &x1, &y1, &tw, &th);
    tft.setTextColor(fg);
    tft.setCursor(x + w - (int16_t)tw - 5 - x1, rowY + 25);   // baseline
    tft.print(txt);
    tft.setFont();   // back to the built-in font for everything else
}

// Line label: "L1", or "L3>L1" when L3 is feeding the L1 output.
static void drawLabel(int r, int8_t src) {
    int16_t y = ROW_Y0 + r * ROW_H;
    int ch = ROW_CH[r];
    tft.fillRect(6, y + 2, LBL_W - 7, ROW_H - 8, COL_BG);
    tft.setTextSize(2);
    tft.setCursor(7, y + 12);
    if (src >= 0) {
        tft.setTextColor(LINE_COLOR[src], COL_BG);
        tft.print(LINE_NAME[src]);
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.print('>');
    }
    tft.setTextColor(LINE_COLOR[ch], COL_BG);
    tft.print(LINE_NAME[ch]);
}

// Format v with `dec` decimals; drop decimals if it would not fit in `maxChars`.
static void fmtFit(char *out, size_t n, float v, int dec, int maxChars) {
    for (int d = dec; d >= 0; d--) {
        snprintf(out, n, "%.*f", d, v);
        if ((int)strlen(out) <= maxChars) return;
    }
    snprintf(out, n, "----");
}

// ---- Network icon (header): WiFi arcs by signal strength, or the AP antenna ----
static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
    uint16_t r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (255 - a)) / 255;
    uint16_t g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a)) / 255;
    uint16_t b = ((fg & 31) * a + (bg & 31) * (255 - a)) / 255;
    return (r << 11) | (g << 5) | b;
}

// Arc of radius r (2 px thick) around (cx, cy), angles in degrees (0 = right, 90 = up).
static void drawArc(int16_t cx, int16_t cy, int16_t r, int a0, int a1, uint16_t c) {
    for (int a = a0; a <= a1; a += 3) {
        float rad = a * (float)M_PI / 180.0f;
        for (int t = 0; t < 2; t++) {
            int16_t px = cx + (int16_t)lroundf((r - t) * cosf(rad));
            int16_t py = cy - (int16_t)lroundf((r - t) * sinf(rad));
            tft.drawPixel(px, py, c);
        }
    }
}

#define NET_X  158   // icon box in the header
#define NET_W  34

// net: 0 = AP only, 1..3 = WiFi connected with 1..3 bars (same thresholds as the web page).
static void drawNetIcon(int8_t net) {
    tft.fillRect(NET_X, 0, NET_W, HDR_H, COL_HDR_BG);
    if (net == 0) {
        int16_t x0 = NET_X + (NET_W - AP_IMG_W) / 2, y0 = (HDR_H - AP_IMG_H) / 2;
        for (int y = 0; y < AP_IMG_H; y++)
            for (int x = 0; x < AP_IMG_W; x++) {
                uint8_t a = pgm_read_byte(&ap_mask[y * AP_IMG_W + x]);
                if (a > 20) tft.drawPixel(x0 + x, y0 + y, blend565(COL_TEXT, COL_HDR_BG, a));
            }
        return;
    }
    int16_t cx = NET_X + NET_W / 2, cy = HDR_H - 5;
    tft.fillCircle(cx, cy, 2, COL_TEXT);
    for (int k = 1; k <= 3; k++)
        drawArc(cx, cy, 5 + k * 5, 45, 135, (net >= k) ? COL_TEXT : COL_OFF);
}

static int8_t netState() {
    if (WiFi.status() != WL_CONNECTED) return 0;
    int32_t rssi = WiFi.RSSI();
    return rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
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
    tft.setCursor(8, COLHDR_Y + 5);
    tft.print("Line");
    for (int c = 0; c < 4; c++) {
        int16_t tw = (int16_t)strlen(COL_NAME[c]) * 6;
        tft.setCursor(COL_X[c] + (COL_W[c] - tw) / 2, COLHDR_Y + 5);
        tft.print(COL_NAME[c]);
    }
    tft.drawFastHLine(0, ROW_Y0 - 3, SCR_W, COL_GRID);

    // Vertical rules between Line | V | A | W | kWh
    for (int c = 0; c < 4; c++)
        tft.drawFastVLine(COL_X[c], COLHDR_Y, FTR_Y - 3 - COLHDR_Y, COL_GRID);

    // Colour tags + row separators (thicker line after the Inverter row, like the web page)
    for (int r = 0; r < 4; r++) {
        int16_t y = ROW_Y0 + r * ROW_H;
        tft.fillRect(2, y + 4, 3, ROW_H - 12, LINE_COLOR[ROW_CH[r]]);
        int16_t ly = y + ROW_H - 4;
        tft.drawFastHLine(0, ly, SCR_W, COL_GRID);
        if (r == 0) tft.drawFastHLine(0, ly + 1, SCR_W, COL_DIM);
        drawLabel(r, -1);
        shownSrc[r] = -1;
    }

    memset(shown, 0, sizeof(shown));
    shownNet = -1;
    shownTime[0] = shownFooter[0] = shownState[0] = '\0';
}

// Power-on check: solid colour fills prove SPI, DC and backlight all work.
__attribute__((unused)) static void colourSelfTest() {
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

// Startup splash: the EVIQtech logo (images/logo.png, all green) with a loading bar.
#define SPLASH_MS    2000UL   // how long the splash stays up
#define LOGO_SCALE   3        // 85 x 24 logo -> 255 x 72 on screen

static void drawCentered(const char *s, int16_t y, uint8_t size, uint16_t color) {
    int16_t w = (int16_t)strlen(s) * 6 * size;   // built-in font: 6 px per char at size 1
    tft.setTextSize(size);
    tft.setTextColor(color, COL_BG);
    tft.setCursor((SCR_W - w) / 2, y);
    tft.print(s);
}

static void drawLogo(int16_t x0, int16_t y0, uint8_t s) {
    for (int y = 0; y < LOGO_IMG_H; y++) {
        for (int x = 0; x < LOGO_IMG_W; x++) {
            uint16_t c = pgm_read_word(&logo_img[y * LOGO_IMG_W + x]);
            if (c) tft.fillRect(x0 + x * s, y0 + y * s, s, s, c);   // black = background
        }
        yield();
    }
}

static void showSplash() {
    tft.fillScreen(COL_BG);

    const int16_t lw = LOGO_IMG_W * LOGO_SCALE, lh = LOGO_IMG_H * LOGO_SCALE;
    const int16_t lx = (SCR_W - lw) / 2, ly = 50;
    drawLogo(lx, ly, LOGO_SCALE);

    tft.drawFastHLine(lx, ly + lh + 10, lw, COL_GRID);
    drawCentered("3-Phase Smart Meter", ly + lh + 20, 2, COL_DIM);

    char ver[24];
    snprintf(ver, sizeof(ver), "FW v%d.%d.%d", FIRMWARE_MAJOR, FIRMWARE_MINOR, FIRMWARE_PATCH);
    drawCentered(ver, 205, 1, COL_DIM);

    // Loading bar (logo green) fills over SPLASH_MS.
    const int16_t bx = 60, by = 188, bw = SCR_W - 120, bh = 6;
    tft.drawRect(bx, by, bw, bh, COL_GRID);
    const int steps = 20;
    for (int i = 1; i <= steps; i++) {
        tft.fillRect(bx + 1, by + 1, (bw - 2) * i / steps, bh - 2, COL_LOGO);
        delay(SPLASH_MS / steps);
        yield();
    }
}

void initDisplay() {
    SPI.begin();
    tft.begin(TFT_SPI_HZ);    // with RST = -1 the library sends a software reset
    tft.setRotation(1);       // landscape, 320 x 240
    // colourSelfTest();      // wiring diagnostic (RGBW fills + MISO readback); enable when debugging
    showSplash();
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
        strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M:%S", &t);
    }
    if (strcmp(buf, shownTime) == 0) return;
    strlcpy(shownTime, buf, sizeof(shownTime));
    int16_t tw = (int16_t)strlen(buf) * 6;
    tft.fillRect(NET_X + NET_W, 0, SCR_W - NET_X - NET_W, HDR_H, COL_HDR_BG);
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
        snprintf(buf, sizeof(buf), "Bat %.2f/3.3V  AP %s", bateryVoltag, WiFi.softAPIP().toString().c_str());
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
    uint16_t rs = relayStatus;

    for (int r = 0; r < 4; r++) {
        int ch = ROW_CH[r];

        // "L3>L1" label when another line feeds this output.
        int8_t src = live ? feedSource(ch, rs) : -1;
        if (src != shownSrc[r]) {
            shownSrc[r] = src;
            drawLabel(r, src);
        }

        float v, i, p;
        readChannel(ch, v, i, p);
        char txt[4][10];
        if (live) {
            fmtFit(txt[0], sizeof(txt[0]), v, 1, 5);
            fmtFit(txt[1], sizeof(txt[1]), i, 2, 5);
            fmtFit(txt[2], sizeof(txt[2]), p, 0, 5);
        } else {
            strcpy(txt[0], "--"); strcpy(txt[1], "--"); strcpy(txt[2], "--");
        }
        fmtFit(txt[3], sizeof(txt[3]), cumEnergy[ch], 2, 7);   // saved value, valid offline too

        int16_t y = ROW_Y0 + r * ROW_H;
        for (int c = 0; c < 4; c++) {
            if (strcmp(txt[c], shown[r][c]) == 0) continue;
            strlcpy(shown[r][c], txt[c], sizeof(shown[r][c]));
            drawCell(COL_X[c], y, COL_W[c], txt[c], COL_TEXT);
            yield();
        }
    }

    int8_t net = netState();
    if (net != shownNet) {
        shownNet = net;
        drawNetIcon(net);
    }
    drawHeaderTime();
    drawFooter(live);
}
