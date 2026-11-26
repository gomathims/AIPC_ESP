#include <Arduino.h>
#include <string.h>
#include <time.h>

#include "preference_management.h"
#include "meter_state.h"
#include "wifi_portal.h"
#include "cloud_mqtt.h"
#include "ota_update.h"
#include "event_log.h"
#include "protection.h"
#include "time_util.h"
#include "ws_events.h"
#include "cumulative_energy.h"
#include "tft_display.h"


// STM32 front-end frame calibration (unchanged from the original bring-up code).
#define CALIB_VOL1 17338.36190
#define CALIB_CUR1 301062.271062
#define CALIB_POW1 648.885586
#define CALIB_ENE1 4667.50266786

#define CALIB_VOL2 17324.5952
#define CALIB_CUR2 307157.509157
#define CALIB_POW2 649.078751
#define CALIB_ENE2 5667.50266786


#define CALIB_VOL3 17319.6761
#define CALIB_CUR3 304084.249084
#define CALIB_POW3 646.745913
#define CALIB_ENE3 5000.96266786

#define CALIB_VOL4 17319.6761
#define CALIB_CUR4 304084.249084
#define CALIB_POW4 646.745913
#define CALIB_ENE4 5000.96266786

//#define CALIB_VOL 1
//#define CALIB_CUR 1
//#define CALIB_POW 1
// #define CALIB_ENE 1



#define YEAR2020 1577836800

// ---- Live metering state (declared extern in meter_state.h) ----------------
 float voltL1 = 0, curL1 = 0, powL1 = 0, eneL1 = 0;
 float voltL2 = 0, curL2 = 0, powL2 = 0, eneL2 = 0;
 float voltL3 = 0, curL3 = 0, powL3 = 0, eneL3 = 0;
 float voltL4 = 0, curL4 = 0, powL4 = 0, eneL4 = 0;
volatile bool meterDataValid = false;
volatile unsigned long meterLastUpdate = 0;
volatile uint16 relayStatus;
// Protection frame to STM: [0]=0xCC, then 13 bytes per phase (L1,L2,L3,Inverter):
// en(1) lowV(4) highV(4) highI(4); then [53..56] = UPS power limit (float W);
// [57] = ~(sum of bytes 1..56). Total 58 bytes.
#define PROT_POWER_OFS (1 + 13 * PROT_PHASES)
#define PROT_FRAME_LEN (PROT_POWER_OFS + 4 + 1)
uint8_t headerProtection=0XCC,protectionDetails[PROT_FRAME_LEN],UARTcommand;
bool protectionDetailsSaved=true;
uint8_t headerTimesynced = 0XDD, timedetail[11];
bool timesynced=false;


typedef struct event{
	time_t carryEventTime;
	uint64_t carryEventFlag;
}event;

event arrEvent[20];
uint8_t structTemp[sizeof(arrEvent[0]) + 2];

uint8_t headerEventReceive = 0XEE;
uint8_t eventWriteIndex = -1;

// Last STM32 event-flag snapshot, used to diff incoming frames into individual
// relay/overload transitions for the web event log. Bits 0-11: level state
// (1 = ON / overload present, 0 = OFF / cleared). Bit 12: one-shot "STM32 restarted" pulse.
static uint32_t lastStmEventFlag = 0;

// Active protection alarms, from event-flag bits 11..15 (see meter_state.h).
volatile uint8_t stmAlarm = 0;

float bateryVoltag;
uint32_t temp;
time_t curTime;





// STM metering payload after "STM\0": 60 bytes (STM transmit[60]); [58] = contactor feedback bits.
static uint8_t rxDataV1[75];
uint8_t contactorStatus = 0;   // bit0 = CF1 ... bit6 = CF7 (1 = feedback active)


static void protectionUartSend(){
    const int last = PROT_FRAME_LEN - 1;   // checksum index (57)
    protectionDetails[0] = headerProtection;
    for (int k = 0; k < PROT_PHASES; k++) {
        uint8_t *p = &protectionDetails[1 + 13 * k];
        p[0] = prot[k].enabled ? 1 : 0;
        memcpy(&p[1], &prot[k].lowV, 4);
        memcpy(&p[5], &prot[k].highV, 4);
        memcpy(&p[9], &prot[k].highI, 4);
    }
    memcpy(&protectionDetails[PROT_POWER_OFS], &prot[PROT_UPS_IDX].highP, 4);   // UPS power limit
    uint8_t sum = 0;
    for (int i = 1; i < last; i++) sum += protectionDetails[i];
    protectionDetails[last] = (uint8_t)~sum;

    Serial.write(protectionDetails, PROT_FRAME_LEN);
    Serial.flush();
    delay(50);
}
static void timeSyncUartSend(){

    time_t now = time(nullptr);
    //  Serial.printf("The unix format time is %llu \r\n",now);                //testing purpose
    if (now > YEAR2020)                                                 // if the year is 2020 only
    { 
        /*testing purpose*/
        //   for (int i = 64; i >= 0; i--)
        // {
        //     Serial.printf("%lld", (now >> i) & 1);
        // }
        // Serial.printf("\r\n");



        timedetail[9] = 0;
        memcpy(&(timedetail[0]), &(headerTimesynced), 1);
        memcpy(&(timedetail[1]), &now, sizeof(now));
        for (int i = 1; i < 9; i++)
        {
            timedetail[9] += timedetail[i];
        }
        timedetail[9]=(~(timedetail[9]&0XFF));

        Serial.flush();
        Serial.write(timedetail,sizeof(timedetail));
        Serial.flush();

        /*testing purpose*/
        // for(int i=0; i<10; i++){
        //     Serial.printf("%d ---->%X\n",i,timedetail[i]);
        // }
        // Serial.printf("\r\n");
    }

    

    
}






static const uint8_t STM_BIT_TO_EV[] = {
    EV_R_MAIN,       // bit 0
    EV_Y_MAIN,       // bit 1
    EV_B_MAIN,       // bit 2
    EV_YR_AUX,       // bit 3
    EV_BR_AUX,       // bit 4
    EV_RY_AUX,       // bit 5
    EV_BY_AUX,       // bit 6
    EV_RB_AUX,       // bit 7
    EV_YB_AUX,       // bit 8
    EV_UPS_MAIN,     // bit 9
    EV_UPS_AUX,      // bit 10
    EV_OVERLOAD_R,   // bit 11
    EV_OVERLOAD_Y,   // bit 12
    EV_OVERLOAD_B,   // bit 13
    EV_OVERLOAD_INV, // bit 14
    EV_OVERPOWER_INV,// bit 15  UPS excess power consumption
};                   // bit 16 = STM restart (STM_EV_RESTART_BIT), handled below

static void processStmEvent(const event &e) {
    uint32_t flag = (uint32_t)e.carryEventFlag;
    uint32_t ts = (uint32_t)e.carryEventTime;
    uint32_t changed = flag ^ lastStmEventFlag;

    for (uint8_t bit = 0; bit < sizeof(STM_BIT_TO_EV); bit++) {
        if (changed & (1UL << bit)) {
            bool active = (flag & (1UL << bit)) != 0;
            logEventAt(STM_BIT_TO_EV[bit], active ? 1 : 0, ts);
        }
    }
    // Restart is a one-shot pulse from the STM32 (set then cleared on its side);
    // only log the rising edge so it doesn't also produce a spurious "cleared" row.
    if ((changed & (1UL << STM_EV_RESTART_BIT)) && (flag & (1UL << STM_EV_RESTART_BIT))) {
        logEventAt(EV_STM_RESTART, 0, ts);
    }

    lastStmEventFlag = flag;
    stmAlarm = (uint8_t)((flag >> 11) & 0x1F);   // overload R/Y/B/INV + INV over power
}

static void uartCollection() {
    while (Serial.available()) {
        // Peek the 4-byte marker without over-consuming on mismatch.
        if (Serial.peek() != 'S') { 
            if(Serial.peek() == 0xEE){
                size_t n = Serial.readBytes(structTemp,sizeof(structTemp));
                eventWriteIndex++;
                if(eventWriteIndex >= 20){
                    eventWriteIndex = 0;
                }
                memcpy(&arrEvent[eventWriteIndex],&structTemp[1],sizeof(arrEvent[0]));
                processStmEvent(arrEvent[eventWriteIndex]);
                // Serial.printf("The time of the Event is %lld and the Event is %lld\r\n",arrEvent->carryEventTime,arrEvent->carryEventFlag);
                break;
            }
            
            uint8_t dropped = Serial.read();
            continue; 
        }

        char ver[4];
        size_t n = Serial.readBytes(ver, 4);
        ver[3] = '\0';
        if (strcmp(ver, "STM") != 0) {
            // Marker didn't line up; the 4 bytes are already consumed, resync next pass.
            continue;
        }

        n = Serial.readBytes(rxDataV1, sizeof(rxDataV1));
        
        memcpy(&voltL1, &rxDataV1,4);
        memcpy(&voltL2, &rxDataV1[4],4);
        memcpy(&voltL3, &rxDataV1[8],4);
        memcpy(&voltL4, &rxDataV1[12],4);
        memcpy(&curL1, &rxDataV1[16],4);
        memcpy(&curL2, &rxDataV1[20],4);
        memcpy(&curL3, &rxDataV1[24],4);
        memcpy(&curL4, &rxDataV1[28],4);
        memcpy(&powL1, &rxDataV1[32],4);
        memcpy(&powL2, &rxDataV1[36],4);
        memcpy(&powL3, &rxDataV1[40],4);
        memcpy(&powL4, &rxDataV1[44],4);
        memcpy(&eneL1, &rxDataV1[48],4);
        memcpy(&eneL2, &rxDataV1[52],4);
        memcpy(&eneL3, &rxDataV1[56],4);
        memcpy(&eneL4, &rxDataV1[60],4);
        relayStatus = ((rxDataV1[65] << 8 | rxDataV1[64]));
        memcpy(&bateryVoltag,&rxDataV1[66],sizeof(bateryVoltag));
        // STM RTC epoch is 4 bytes at [42..45]. time_t is 8 bytes on the ESP8266 core,
        // so copy into a uint32_t first (copying sizeof(time_t) would read past the buffer).
        uint32_t stmEpoch;
        memcpy(&stmEpoch, &rxDataV1[70], sizeof(stmEpoch));
        curTime = (time_t)stmEpoch;
        contactorStatus = rxDataV1[74];   // CF1..CF7 feedback bits ([59] unused)

        // Serial.printf("The voltage is %.2f\r\n",bateryVoltag);

       
        meterDataValid = true;
        meterLastUpdate = millis();
        updateCumulativeEnergy();   // accumulate L1-L4 energy; saved to flash hourly
    }
    // while(Serial.available() == 1){
    //     Serial.readBytes(&UARTcommand,1);
    //    if(UARTcommand==headerProtection){
    //     protectionDetailsSaved = true;
    //    }

    // }

    
}

void setup() {
    Serial.setRxBufferSize(256);   // hold >1 STM frame while WiFi is busy
    Serial.begin(115200);
    Serial.flush();
    Serial.setTimeout(50);
    // Serial.println("setup started");
    delay(100);


    // WiFi web portal (AP + station in parallel), cloud, OTA, event log.
    initPreferences();
    initProtection();
    initCumulativeEnergy();
    initDisplay();          // 2.4" ILI9341 TFT: colour self-test, then the monitoring screen
    initEventLog();
    initWiFiPortal();
    initWsEvents();
    initCloud();
    initOTA();
    confirmFirmwareValid();
}

void loop() {
    

    

    // Decode any complete metering frames from the STM front-end.
    // if(!temp){
    //     temp=millis();
    // }
    // if((millis() - temp) >500){
    //     temp=0;
    // }
    uartCollection();
    //Sending protection details through uart towards STM
    if(protectionDetailsSaved){
    protectionUartSend();
    protectionDetailsSaved = false;
    }
    if(timesynced){
        timeSyncUartSend();
        timesynced=false;
    }
    // Non-blocking WiFi housekeeping (handleClient + reconnect/AP retry).
    serviceWiFiPortal();
    // WebSocket: push live readings / WiFi / cloud status to the web pages.
    serviceWsEvents();
    // Cloud (MQTT) housekeeping; active only when WiFi is connected + cloud enabled.
    serviceCloud();
    // Cloud OTA: download+flash+reboot when a newer firmware was auto-assigned.
    if (otaState == OTA_DOWNLOADING) performOTAUpdate();
    // Record the boot/restart event to flash once the clock is available.
    serviceEventLog();
    // Refresh the TFT phase-monitoring screen (only changed values are repainted).
    serviceDisplay();
}
