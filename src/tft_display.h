#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

// 2.4" 240x320 SPI TFT (ILI9341 controller) — live phase monitoring screen.
//
// Wiring (hardware SPI = HSPI, the pins on the SIDE of the ESP-12F):
//   Display SCK        -> GPIO14
//   Display SDI (MOSI) -> GPIO13
//   Display SDO (MISO) -> GPIO12   (optional; used only by the self-test readback)
//   Display CS         -> GPIO5
//   Display DC / RS    -> GPIO4
//   Display RESET      -> 3.3 V through 10k, or the ESP EN/RST pin (not driven by code)
//   VCC + LED -> 5 V, GND -> GND
// The ESP-12F still needs GPIO15 pulled LOW (10k to GND) to boot.
//
// DO NOT use the ESP-12F bottom-edge pins CS0, MISO, GPIO9, GPIO10, MOSI, SCLK:
// they are the module's internal SPI-flash bus. A display there listens to flash
// traffic (stays white) and its SDO output corrupts flash reads (ESP crashes).

void initDisplay();      // call once in setup(); runs a short colour self-test.
void serviceDisplay();   // call from loop(); redraws changed values ~2x per second.

#endif // TFT_DISPLAY_H
