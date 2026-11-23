#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

// 2.4" 240x320 SPI TFT (ILI9341 controller) — live phase monitoring screen.
//
// Wiring (hardware SPI / HSPI):
//   SCLK -> GPIO14   MISO -> GPIO12   MOSI -> GPIO13   CS -> GPIO15 (CS0)
//   DC   -> GPIO10   RESET -> GPIO9   VCC + LED -> 5 V
//
// GPIO9 / GPIO10 are SPI-flash pins on the ESP-12 module:
//   - GPIO10 is only free when the flash runs in DIO mode (set in platformio.ini).
//   - GPIO9 is the flash HOLD line even in DIO mode; driving it LOW freezes the
//     flash and crashes the ESP. So by default the reset pin is NOT driven
//     (TFT_RST = -1) and the panel is reset by the ILI9341 software-reset command.
//     If your board really has GPIO9 free, build with -D TFT_RST_PIN=9.

void initDisplay();      // call once in setup(); runs a short colour self-test.
void serviceDisplay();   // call from loop(); redraws changed values ~2x per second.

#endif // TFT_DISPLAY_H
