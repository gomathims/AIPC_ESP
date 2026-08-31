#ifndef STM_OTA_H
#define STM_OTA_H

#include <Arduino.h>

// ---------------------------------------------------------------------------
// STM32 firmware update over the metering UART, using a CUSTOM bootloader in
// STM32 flash (not the ST ROM bootloader).
//
// Because the receiver is your own code, this needs none of what the ROM
// bootloader demanded: no option-byte writes, no nBOOT0/nSWBOOT0, no reset into
// system memory, and no 8E1 parity switch. The link stays 115200 8N1 throughout
// and works on any UART, including USART3 / PB10-PB11.
//
// Wire protocol
// -------------
//   ESP -> STM   0xAA                    update request
//   STM -> ESP   0xAB                    ready, bootloader is listening
//   ESP -> STM   size[4], little-endian  image length in bytes
//   ESP -> STM   <size bytes>            raw firmware, streamed in chunks
//   STM -> ESP   0xAC | 0xAE             (optional) final success / failure
//
// The 4-byte size is sent because the receiver has no other way to know when
// the image ends — there is no end marker and no idle timeout in this protocol.
// If your bootloader derives the length differently, drop STM_UPD_SEND_SIZE.
// ---------------------------------------------------------------------------

// Stage 1 — staging. The RUNNING APP receives the image and programs it into
// flash bank 2 while it executes from bank 1. Nothing it depends on is erased,
// so a failure here costs nothing and can simply be retried.
// Commands are 4-byte signatures, NOT single bytes. Both of them erase flash,
// and the STM32's RX line floats whenever the ESP is resetting, powering up or
// being reflashed — a floating line plus the ESP8266's 74880-baud ROM banner
// readily produces stray 0xAA/0xBB bytes at 115200. A single noise byte must
// never be able to erase the front-end.
#define STM_UPD_REQ_SEQ    {0xAA, 0x55, 0xA5, 0x5A}
#define STM_UPD_APPLY_SEQ  {0xBB, 0x44, 0xB4, 0x4B}
#define STM_UPD_SEQ_LEN    4

#define STM_UPD_REQ        0xAA   // ESP -> STM: "prepare to receive an image"
#define STM_UPD_READY      0xAB   // STM -> ESP: "bank 2 erased, send it"
#define STM_UPD_DONE_OK    0xAC   // STM -> ESP: image stored in bank 2
#define STM_UPD_DONE_FAIL  0xAE   // STM -> ESP: staging failed

// Stage 2 — apply. Triggered separately by the user. The app sets its boot
// magic and resets; the bootloader copies bank 2 over the app region and jumps.
// No UART is involved in the copy, so the risky window is ~2 s instead of ~10 s.
#define STM_UPD_APPLY      0xBB   // ESP -> STM: "reboot and install what I sent"

// Send the 4-byte little-endian length before the payload. Set to 0 if your
// bootloader already knows the size or uses its own terminator.
#define STM_UPD_SEND_SIZE  1

// Wait for STM_UPD_DONE_OK after the last byte. Set to 0 if your bootloader
// jumps straight to the application without reporting back.
#define STM_UPD_WAIT_FINAL 1

// Wait for STM_UPD_READY (0xAB) after each chunk as well. Off by default: the
// plain flow blasts the image continuously. Turn this on if the STM32 cannot
// erase/write fast enough and starts dropping bytes mid-transfer.
#define STM_UPD_CHUNK_ACK  0

#define STM_UPD_CHUNK      256    // bytes per write burst

// Where the STM32 application is linked to run. Used to sanity-check the
// uploaded image's reset vector before sending it. Change this if the app moves
// (e.g. to 0x08008000 if you later add a separate bootloader).
#define STM_APP_BASE       0x08000000UL

// Generous on purpose: the bootloader must erase the whole app region BEFORE it
// answers 0xAB (it cannot erase mid-transfer without dropping bytes), and
// erasing ~112 pages on a G4 takes roughly 3 s.
#define STM_UPD_READY_TIMEOUT_MS  15000U  // wait for 0xAB after the request
#define STM_UPD_ATTEMPTS          3       // resend the request this many times
#define STM_UPD_CHUNK_TIMEOUT_MS  2000U   // per-chunk ACK, when enabled
#define STM_UPD_FINAL_TIMEOUT_MS  10000U  // final status (allows a slow last write)

// ---- Result of an update attempt ------------------------------------------
enum StmOtaResult : uint8_t {
    STM_OTA_OK = 0,
    STM_OTA_NO_FILE,      // staged image missing or empty
    STM_OTA_NO_READY,     // STM32 never sent 0xAB
    STM_OTA_READ_FAILED,  // could not read the staged image back
    STM_OTA_SEND_FAILED,  // chunk ACK timed out mid-transfer
    STM_OTA_REJECTED,     // STM32 reported failure, or never confirmed
    STM_OTA_NO_RESTART,   // apply sent, but metering frames never came back
    STM_OTA_BAD_IMAGE,    // .bin's vector table is wrong for STM_APP_BASE
};

const char *stmOtaResultName(StmOtaResult r);

// How long to wait after 0xBB for the new firmware to start streaming frames
// again (bootloader copy + app startup).
#define STM_APPLY_TIMEOUT_MS 15000U

// Progress + last outcome, for the web UI.
extern volatile uint8_t stmOtaProgress;     // 0-100
extern volatile bool    stmOtaInProgress;
extern char             stmOtaMessage[96];

// Stage 1: stream an image staged in LittleFS into the STM32's bank 2. Blocking.
StmOtaResult stmOtaFlashFromFile(const char *path);

// Stage 2: tell the STM32 to reboot and install what was staged, then wait for
// metering frames to prove the new firmware came up. Blocking.
StmOtaResult stmOtaApply();

#endif // STM_OTA_H
