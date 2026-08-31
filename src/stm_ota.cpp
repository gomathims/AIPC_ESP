#include "stm_ota.h"

#include <LittleFS.h>

volatile uint8_t stmOtaProgress = 0;
volatile bool    stmOtaInProgress = false;
char             stmOtaMessage[96] = {0};

const char *stmOtaResultName(StmOtaResult r) {
    switch (r) {
        case STM_OTA_OK:          return "OK";
        case STM_OTA_NO_FILE:     return "no firmware staged";
        case STM_OTA_NO_READY:    return "STM32 did not answer the update request";
        case STM_OTA_READ_FAILED: return "could not read staged image";
        case STM_OTA_SEND_FAILED: return "transfer stalled - STM32 stopped acknowledging";
        case STM_OTA_REJECTED:    return "STM32 rejected the image";
        case STM_OTA_NO_RESTART:  return "STM32 did not restart after apply";
        case STM_OTA_BAD_IMAGE:   return "firmware built for the wrong address";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Low-level helpers
// ---------------------------------------------------------------------------
static void drainRx() {
    while (Serial.available()) Serial.read();
}

// Read one byte within `timeoutMs`. Returns -1 on timeout. yield() keeps WiFi
// serviced and the soft watchdog fed during the long waits.
static int readByte(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (Serial.available()) return Serial.read();
        yield();
    }
    return -1;
}

// Scan for a specific byte, discarding anything else that arrives first — a
// metering frame already in flight can land ahead of the reply.
static bool waitFor(uint8_t want, uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        int b = readByte(100);
        if (b == want) return true;
        yield();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Step 1 — ask the STM32 to enter its bootloader and wait for 0xAB
// ---------------------------------------------------------------------------
static bool requestUpdate() {
    static const uint8_t seq[STM_UPD_SEQ_LEN] = STM_UPD_REQ_SEQ;

    for (uint8_t attempt = 0; attempt < STM_UPD_ATTEMPTS; attempt++) {
        drainRx();                          // discard buffered metering frames
        Serial.write(seq, STM_UPD_SEQ_LEN);
        Serial.flush();

        if (waitFor(STM_UPD_READY, STM_UPD_READY_TIMEOUT_MS)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Full sequence
// ---------------------------------------------------------------------------
StmOtaResult stmOtaFlashFromFile(const char *path) {
    stmOtaInProgress = true;
    stmOtaProgress = 0;
    stmOtaMessage[0] = '\0';

    File f = LittleFS.open(path, "r");
    if (!f || f.size() == 0) {
        if (f) f.close();
        stmOtaInProgress = false;
        return STM_OTA_NO_FILE;
    }
    const uint32_t total = f.size();

    // ---- 0. Sanity-check the image before it goes anywhere near the STM32 ---
    // A Cortex-M image begins with {initial SP, reset handler}. Checking them
    // here catches the classic mistake of uploading a .bin built for a
    // different link address, which the STM32 cannot reliably detect on its own
    // and which leaves it unbootable.
    {
        uint8_t vt[8];
        if (f.read(vt, 8) != 8) {
            f.close();
            stmOtaInProgress = false;
            return STM_OTA_READ_FAILED;
        }
        f.seek(0);

        uint32_t sp = (uint32_t)vt[0] | ((uint32_t)vt[1] << 8) |
                      ((uint32_t)vt[2] << 16) | ((uint32_t)vt[3] << 24);
        uint32_t pc = (uint32_t)vt[4] | ((uint32_t)vt[5] << 8) |
                      ((uint32_t)vt[6] << 16) | ((uint32_t)vt[7] << 24);

        bool spOk = (sp >= 0x20000000UL && sp <= 0x20020000UL);
        // The reset handler must sit inside the image itself, at its intended
        // load address. A .bin linked for 0x08008000 fails here.
        bool pcOk = (pc >= STM_APP_BASE && pc < (STM_APP_BASE + total) && (pc & 1U));

        if (!spOk || !pcOk) {
            f.close();
            snprintf(stmOtaMessage, sizeof(stmOtaMessage),
                     "bad image: sp=0x%08X pc=0x%08X (wrong link address?)",
                     (unsigned)sp, (unsigned)pc);
            stmOtaInProgress = false;
            return STM_OTA_BAD_IMAGE;
        }
    }

    // ---- 1. Request + wait for ready --------------------------------------
    if (!requestUpdate()) {
        f.close();
        stmOtaInProgress = false;
        return STM_OTA_NO_READY;
    }

    StmOtaResult result = STM_OTA_OK;

#if STM_UPD_SEND_SIZE
    // ---- 2. Image length, little-endian -----------------------------------
    uint8_t len[4] = {
        (uint8_t)(total),
        (uint8_t)(total >> 8),
        (uint8_t)(total >> 16),
        (uint8_t)(total >> 24),
    };
    Serial.write(len, sizeof(len));
    Serial.flush();
#endif

    // ---- 3. Stream the image ----------------------------------------------
    // static, not stack: this runs inside the web-server handler where the WiFi
    // stack has already consumed most of the available stack space.
    static uint8_t buf[STM_UPD_CHUNK];
    uint32_t sent = 0;

    while (sent < total) {
        int n = f.read(buf, STM_UPD_CHUNK);
        if (n <= 0) { result = STM_OTA_READ_FAILED; break; }

        Serial.write(buf, n);
        Serial.flush();          // block until the burst is physically out

#if STM_UPD_CHUNK_ACK
        if (!waitFor(STM_UPD_READY, STM_UPD_CHUNK_TIMEOUT_MS)) {
            result = STM_OTA_SEND_FAILED;
            break;
        }
#endif

        sent += n;
        stmOtaProgress = (uint8_t)((sent * 100UL) / total);
        yield();                 // service WiFi between bursts
    }

    f.close();

    // ---- 4. Final status ---------------------------------------------------
#if STM_UPD_WAIT_FINAL
    if (result == STM_OTA_OK) {
        if (!waitFor(STM_UPD_DONE_OK, STM_UPD_FINAL_TIMEOUT_MS)) {
            result = STM_OTA_REJECTED;
        }
    }
#endif

    drainRx();

    snprintf(stmOtaMessage, sizeof(stmOtaMessage), "%s (%u bytes)",
             stmOtaResultName(result), (unsigned)total);
    if (result == STM_OTA_OK) stmOtaProgress = 100;
    stmOtaInProgress = false;
    return result;
}

// ---------------------------------------------------------------------------
// Stage 2 — apply
//
// Sends 0xBB. The app sets its boot magic and resets; the bootloader copies
// bank 2 over the app region and jumps to it. Success is judged by the metering
// stream coming back, since the freshly started app has no other way to report
// in and the bootloader itself never speaks UART in this design.
// ---------------------------------------------------------------------------
StmOtaResult stmOtaApply() {
    stmOtaInProgress = true;
    snprintf(stmOtaMessage, sizeof(stmOtaMessage), "applying...");

    static const uint8_t applySeq[STM_UPD_SEQ_LEN] = STM_UPD_APPLY_SEQ;
    drainRx();
    Serial.write(applySeq, STM_UPD_SEQ_LEN);
    Serial.flush();

    // The app answers 0xAC immediately before it erases and reprograms itself.
    // Waiting for that first matters: without it, a metering frame that was
    // already queued when 0xBB arrived looks exactly like the new firmware
    // coming back, and every attempt reports success regardless of outcome.
    if (!waitFor(STM_UPD_DONE_OK, STM_UPD_READY_TIMEOUT_MS)) {
        snprintf(stmOtaMessage, sizeof(stmOtaMessage),
                 "front-end did not accept the install (nothing staged?)");
        stmOtaInProgress = false;
        return STM_OTA_REJECTED;
    }

    // Anything from here on can only come from the reprogrammed application.
    uint8_t match = 0;
    const char marker[3] = {'S', 'T', 'M'};
    uint32_t start = millis();

    while (millis() - start < STM_APPLY_TIMEOUT_MS) {
        int b = readByte(200);
        if (b < 0) { yield(); continue; }
        match = (b == (int)(uint8_t)marker[match]) ? (match + 1) : 0;
        if (match == 3) {
            snprintf(stmOtaMessage, sizeof(stmOtaMessage), "applied - front-end restarted");
            stmOtaInProgress = false;
            return STM_OTA_OK;
        }
        yield();
    }

    snprintf(stmOtaMessage, sizeof(stmOtaMessage), "%s", stmOtaResultName(STM_OTA_NO_RESTART));
    stmOtaInProgress = false;
    return STM_OTA_NO_RESTART;
}
