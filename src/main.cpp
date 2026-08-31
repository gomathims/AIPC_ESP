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


// STM32 front-end frame calibration (unchanged from the original bring-up code).
#define CALIB_VOL1 14324.957805
#define CALIB_CUR1 301062.271062
#define CALIB_POW1 648.885586
#define CALIB_ENE1 4667.50266786

#define CALIB_VOL2 14374.3938844
#define CALIB_CUR2 345157.509157
#define CALIB_POW2 739.078751
#define CALIB_ENE2 5667.50266786


#define CALIB_VOL3 14388.8458298
#define CALIB_CUR3 304084.249084
#define CALIB_POW3 656.745913
#define CALIB_ENE3 5000.96266786

//#define CALIB_VOL 1
//#define CALIB_CUR 1
//#define CALIB_POW 1
// #define CALIB_ENE 1



#define YEAR2020 1577836800

// ---- Live metering state (declared extern in meter_state.h) ----------------
volatile float voltL1 = 0, curL1 = 0, powL1 = 0, eneL1 = 0;
volatile float voltL2 = 0, curL2 = 0, powL2 = 0, eneL2 = 0;
volatile float voltL3 = 0, curL3 = 0, powL3 = 0, eneL3 = 0;
volatile bool meterDataValid = false;
volatile unsigned long meterLastUpdate = 0;
volatile uint16 relayStatus;
uint8_t headerProtection=0XCC,protectionDetails[41],UARTcommand;
bool protectionDetailsSaved=true;
uint8_t headerTimesynced = 0XDD, timedetail[11];
bool timesynced=false;





static uint8_t rxDataV1[38];

// void uartCollection()
// {
//   char ver[4];
//   Serial.readBytes(ver, 4);
//   delay(1);
//   // printf("the verification bytes are %s\n",ver);
//   if (strcmp(ver, "STM") == 0)
//   {
//     if (Serial.available() >= 36)
//     {
//       Serial.readBytes(rxDataV1, 36);
//       voltL1 = ((rxDataV1[0] << 16 | rxDataV1[1] << 8 | rxDataV1[2]) / CALIB_VOL);
//       voltL2 = ((rxDataV1[3] << 16 | rxDataV1[4] << 8 | rxDataV1[5]) / CALIB_VOL);
//       voltL3 = ((rxDataV1[6] << 16 | rxDataV1[7] << 8 | rxDataV1[8]) / CALIB_VOL);
//       curL1 = ((rxDataV1[9] << 16 | rxDataV1[10] << 8 | rxDataV1[11]) / CALIB_CUR);
//       curL2 = ((rxDataV1[12] << 16 | rxDataV1[13] << 8 | rxDataV1[14]) / CALIB_CUR);
//       curL3 = ((rxDataV1[15] << 16 | rxDataV1[16] << 8 | rxDataV1[17]) / CALIB_CUR);
//       powL1 = ((rxDataV1[18] << 16 | rxDataV1[19] << 8 | rxDataV1[20]) / CALIB_POW);
//       powL2 = ((rxDataV1[21] << 16 | rxDataV1[22] << 8 | rxDataV1[23]) / CALIB_POW);
//       powL3 = ((rxDataV1[24] << 16 | rxDataV1[25] << 8 | rxDataV1[26]) / CALIB_POW);
//       eneL1 = ((rxDataV1[27] << 16 | rxDataV1[28] << 8 | rxDataV1[29]) / CALIB_ENE);
//       eneL2 = ((rxDataV1[30] << 16 | rxDataV1[31] << 8 | rxDataV1[32]) / CALIB_ENE);
//       eneL3 = ((rxDataV1[33] << 16 | rxDataV1[34] << 8 | rxDataV1[35]) / CALIB_ENE);
//       // printf("H->%d  M->%d  L->%d  H->%d  M->%d  L->%d H->%d  M->%d  L->%d H->%d  M->%d  L->%d H->%d\n", rxDataV1[0], rxDataV1[1], rxDataV1[2], rxDataV1[3], rxDataV1[4], rxDataV1[5], rxDataV1[6], rxDataV1[7], rxDataV1[8], rxDataV1[9], rxDataV1[10], rxDataV1[11], rxDataV1[12]);
//     }
//   }
// }

static void protectionUartSend(){
    memcpy((&protectionDetails[0]),&headerProtection,1);
    memcpy((&protectionDetails[1]),&prot[0].enabled,1);
    memcpy((&protectionDetails[2]),&prot[0].lowV,4);
    memcpy((&protectionDetails[6]),&prot[0].highV,4);
    memcpy((&protectionDetails[10]),&prot[0].highI,4);
    memcpy((&protectionDetails[14]),&prot[1].enabled,1);
    memcpy((&protectionDetails[15]),&prot[1].lowV,4);
    memcpy((&protectionDetails[19]),&prot[1].highV,4);
    memcpy((&protectionDetails[23]),&prot[1].highI,4);
    memcpy((&protectionDetails[27]),&prot[2].enabled,1);
    memcpy((&protectionDetails[28]),&prot[2].lowV,4);
    memcpy((&protectionDetails[32]),&prot[2].highV,4);
    memcpy((&protectionDetails[36]),&prot[2].highI,4);
    for(int i=1;i<40;i++){
        protectionDetails[40]+=protectionDetails[i];
    }
    protectionDetails[40]=(~(protectionDetails[40]&0XFF));
    // Serial.printf("\nchecksum------------------->%x\n",protectionDetails[39]);

        Serial.write(protectionDetails,41);
        Serial.flush();
        delay(50);
     

        // Serial.printf(" prot L1 %d ->%f  %f  %f   \n",prot[0].enabled,prot[0].lowV,prot[0].highV,prot[0].highI);
        // Serial.printf(" prot L1 %d ->%f  %f  %f   \n",prot[1].enabled,prot[1].lowV,prot[1].highV,prot[1].highI);
        // Serial.printf(" prot L1 %d ->%f  %f  %f   \n",prot[2].enabled,prot[2].lowV,prot[2].highV,prot[2].highI);


    protectionDetails[40] = 0;

}
static void timeSyncUartSend(){

    time_t now = time(nullptr);
     printf("The unix format time is %llu \r\n",now);                //testing purpose
    if (now > YEAR2020)                                                 // if the year is 2020 only
    { 
        /*testing purpose*/
        //   for (int i = 64; i >= 0; i--)
        // {
        //     Serial.printf("%lld", (now >> i) & 1);
        // }
        // Serial.printf("\r\n");



        timedetail[10] = 0;
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




// Non-blocking UART reader: only consumes a frame once a full one is buffered,
// so it never stalls the WiFi/cloud service loop. On a header mismatch it drops
// a single byte to resynchronise with the stream.
static void uartCollection() {
    while (Serial.available() >= 42) {
        // Peek the 4-byte marker without over-consuming on mismatch.
        if (Serial.peek() != 'S') { Serial.read(); continue; }

        char ver[4];
        Serial.readBytes(ver, 4);
        ver[3] = '\0';
        if (strcmp(ver, "STM") != 0) {
            // Marker didn't line up; the 4 bytes are already consumed, resync next pass.
            continue;
        }

        Serial.readBytes(rxDataV1, sizeof(rxDataV1));
        voltL1 = ((rxDataV1[0] << 16 | rxDataV1[1]  << 8 | rxDataV1[2]) / CALIB_VOL1);
        voltL2 = ((rxDataV1[3] << 16 | rxDataV1[4]  << 8 | rxDataV1[5]) / CALIB_VOL2);
        voltL3 = ((rxDataV1[6] << 16 | rxDataV1[7]  << 8 | rxDataV1[8]) / CALIB_VOL3);
        curL1 = ((rxDataV1[9]  << 16 | rxDataV1[10] << 8 | rxDataV1[11]) / CALIB_CUR1);
        curL2 = ((rxDataV1[12] << 16 | rxDataV1[13] << 8 | rxDataV1[14]) / CALIB_CUR2);
        curL3 = ((rxDataV1[15] << 16 | rxDataV1[16] << 8 | rxDataV1[17]) / CALIB_CUR3);
        powL1 = ((rxDataV1[18] << 16 | rxDataV1[19] << 8 | rxDataV1[20]) / CALIB_POW1);
        powL2 = ((rxDataV1[21] << 16 | rxDataV1[22] << 8 | rxDataV1[23]) / CALIB_POW2);
        powL3 = ((rxDataV1[24] << 16 | rxDataV1[25] << 8 | rxDataV1[26]) / CALIB_POW3);
        eneL1 = ((rxDataV1[27] << 16 | rxDataV1[28] << 8 | rxDataV1[29]) / CALIB_ENE1);
        eneL2 = ((rxDataV1[30] << 16 | rxDataV1[31] << 8 | rxDataV1[32]) / CALIB_ENE2);
        eneL3 = ((rxDataV1[33] << 16 | rxDataV1[34] << 8 | rxDataV1[35]) / CALIB_ENE3);
        relayStatus = ((rxDataV1[37] << 8 | rxDataV1[36]));
        if (curL1 <= 0.08) {
			curL1 = 0;
			powL1 = 0;
		}
		if (curL2 <= 0.08) {
			curL2 = 0;
			powL2 = 0;
		}
		if (curL3 <= 0.08) {
			curL3 = 0;
			powL3 = 0;
		}
        // Serial.printf("volt =%f\nrelay status  %x\n",voltL1,relayStatus);
       
        meterDataValid = true;
        meterLastUpdate = millis();
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
    Serial.println("setup started");
    delay(100);


    // WiFi web portal (AP + station in parallel), cloud, OTA, event log.
    initPreferences();
    initProtection();
    initEventLog();
    initWiFiPortal();
    initCloud();
    initOTA();
    confirmFirmwareValid();
}

void loop() {
    

    

    // Decode any complete metering frames from the STM front-end.
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
    // Cloud (MQTT) housekeeping; active only when WiFi is connected + cloud enabled.
    serviceCloud();
    // Cloud OTA: download+flash+reboot when a newer firmware was auto-assigned.
    if (otaState == OTA_DOWNLOADING) performOTAUpdate();
    // Record the boot/restart event to flash once the clock is available.
    serviceEventLog();

}
