// WATERNODE FIRMWARE - MAIN SKETCH
// Low-Cost Water Quality Monitoring System for Rural Nepal
// Dublin City University - Final Year Project
// Author: Jedidiah Olagbemiro
//
// REQUIRED LIBRARIES (install via Arduino Library Manager):
//   - OneWire by Paul Stoffregen
//   - DallasTemperature by Miles Burton
//   - RTClib by Adafruit
//   - Adafruit ADS1X15 (pulls in Adafruit BusIO as a dependency)
//   - SD (built-in Arduino/ESP32 core)
//
// HARDWARE:
//   - ESP32-WROOM-32 DevKit v1 (38-pin, genuine CH340G)
//   - DFRobot SEN0189 analog turbidity sensor (5V, via MT3608 boost)
//   - DFRobot SEN0244 analog TDS sensor
//   - DS18B20 waterproof temperature probe (genuine Maxim)
//   - DS3231 RTC module (I2C)
//   - ADS1115 external 16-bit ADC (I2C, shares bus with DS3231), turbidity and
//     TDS are read through this rather than the ESP32's internal ADC
//   - SIM800L Mini v2.0 GSM module (900MHz band for Nepal Telecom)
//   - MicroSD card module with onboard 3.3V LDO
//   - MT3608 boost converter module (battery 3.7V -> 5V regulated)
//   - BSS138 N-channel MOSFET SOT-23 (sensor power switching, GPIO27, low-side)
//   - TP4056 + DW01A Li-Ion charger/protector
//   - Samsung 30Q 18650 cell (genuine, 3000mAh)
//   - 100k ohm/100k ohm voltage divider on turbidity signal before ADS1115 A0
//   - 100k ohm/100k ohm voltage divider on battery ADC (GPIO36)
//   - 470uF low-ESR + 100uF tantalum decoupling on SIM800L power rail

#include <Arduino.h>
#include "config.h"
#include "sensors.h"
#include "rtc_module.h"
#include "storage.h"
#include "connectivity.h"
#include "alert.h"

// Forward declaration, defined at bottom of file
void goToSleep(bool fastMode);

// RTC MEMORY. Survives ESP32 deep sleep
// All state that must persist across sleep cycles lives here
RTC_DATA_ATTR bool     firstBoot         = true;
RTC_DATA_ATTR bool     rtcSynced         = false;
RTC_DATA_ATTR uint32_t lastTransmitTime  = 0;
RTC_DATA_ATTR bool     samplingFastMode  = false;    // true = 5min anomaly mode
RTC_DATA_ATTR uint32_t anomalyStartTime  = 0;        // When fast mode began
RTC_DATA_ATTR AlertState alertState;                 // Full alert state (all parameters)
RTC_DATA_ATTR int      bootCount         = 0;        // Total wake cycles since power-on

// SETUP. Runs every wake from deep sleep
// This is the entire program, loop() is never reached
void setup() {
    Serial.begin(115200);
    delay(100);

    bootCount++;
    Serial.printf("\n========== WaterNode Boot #%d ==========\n", bootCount);
    Serial.printf("Firmware v%s | Node: %s\n", FIRMWARE_VERSION, NODE_ID);

    // --- Status LED on during active phase ---
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, HIGH);

    // 1. INITIALISE RTC
    bool rtcOk = rtcInit();
    uint32_t currentTime = rtcOk ? rtcGetUnixTime() : 0;
    int currentMonth = rtcOk ? rtcGetMonth() : 1;
    int seasonIndex  = getSeasonIndex(currentMonth);

    Serial.printf("Season: %d | Unix time: %u\n", seasonIndex, currentTime);

    // 2. FIRST BOOT INITIALISATION
    if (firstBoot) {
        Serial.println("[BOOT] First boot - initialising state");
        alertInit(&alertState, currentTime);
        lastTransmitTime = currentTime;
        firstBoot = false;
        bootCount = 1;
    }

    // 3. CHECK ANOMALY MODE TIMEOUT
    // After ANOMALY_RECOVERY_MINUTES of fast sampling, revert to normal
    if (samplingFastMode && currentTime > 0) {
        uint32_t anomalyDuration = (currentTime - anomalyStartTime) / 60;
        if (anomalyDuration > ANOMALY_RECOVERY_MINUTES) {
            samplingFastMode = false;
            Serial.println("[SAMPLE] Anomaly recovery period elapsed - normal sampling");
        }
    }

    // 4. INITIALISE SD CARD
    bool sdOk = storageInit();
    if (!sdOk) {
        Serial.println("[SD] WARNING: SD card unavailable - data will not be logged locally");
    }

    // 5. READ SENSORS
    sensorsInit();
    sensorsOn();

    Serial.println("[SENSORS] Reading...");
    SensorReading reading = readAllSensors();

    sensorsOff();

    // If RTC failed, record that fault in the reading's fault flags
    // so it persists in the CSV and is visible to the backend
    if (!rtcOk) {
        reading.fault_flags |= FAULT_RTC_FAIL;
        Serial.println("[RTC] FAULT_RTC_FAIL set - timestamp unreliable");
    }

    // Log sensor readings to Serial for debugging
    Serial.printf("[SENSORS] Turbidity: %.2f NTU (raw: %d)\n",
                  reading.turbidity_ntu, reading.turbidity_raw);
    Serial.printf("[SENSORS] TDS: %.2f mg/L\n", reading.tds_mgl);
    Serial.printf("[SENSORS] Temperature: %.2f C\n", reading.temperature_c);
    Serial.printf("[SENSORS] Battery: %d mV\n", reading.battery_mv);
    Serial.printf("[SENSORS] Faults: 0x%02X\n", reading.fault_flags);

    // 6. EVALUATE ALERTS (local threshold check)
    int alertLevel = alertEvaluate(&alertState, reading, seasonIndex, currentTime);
    Serial.printf("[ALERT] Level: %s (%d)\n", alertLevelString(alertLevel), alertLevel);

    // Enter/exit fast sampling mode based on alert level
    if (alertLevel >= ALERT_WARNING && !samplingFastMode) {
        samplingFastMode = true;
        anomalyStartTime = currentTime;
        Serial.println("[SAMPLE] Entering fast-sample mode (5 min)");
    }

    // 7. WRITE TO SD BUFFER
    if (sdOk) {
        bool writeOk = storageWriteReading(currentTime, reading,
                                           alertLevel, seasonIndex);
        if (!writeOk) {
            Serial.println("[SD] Buffer write failed");
            reading.fault_flags |= FAULT_SD_WRITE;
        }
    }

    // 8. CHECK BATTERY. Critical: skip GPRS to conserve power
    bool batteryOk = (reading.battery_mv > BATTERY_CRITICAL_MV);
    if (!batteryOk) {
        Serial.printf("[POWER] CRITICAL battery (%d mV) - skipping GPRS\n",
                      reading.battery_mv);
        storageLogEvent(currentTime, "Critical battery - GPRS skipped");
        // SMS battery warning, uses less power than GPRS
        char battMsg[120];
        snprintf(battMsg, sizeof(battMsg),
                 "BATTERY CRITICAL: Node %s battery at %d mV. Immediate attention required.",
                 NODE_ID, reading.battery_mv);
        // Only send battery SMS every 4 hours to avoid drain
        if ((currentTime - lastTransmitTime) >= (TRANSMIT_INTERVAL_HOURS * 3600)) {
            simInit();
            simWaitBoot();
            smsSendAlert(battMsg);
            lastTransmitTime = currentTime;  // Prevent re-sending every wake cycle
            simSleep();     // Return SIM800L to ~0.7mA before deep sleep
        }
        goToSleep(samplingFastMode);
        return;
    }

    // 9. GPRS UPLOAD (every TRANSMIT_INTERVAL_HOURS)
    bool transmitDue = (currentTime - lastTransmitTime) >=
                       (uint32_t)(TRANSMIT_INTERVAL_HOURS * 3600);

    if (transmitDue && sdOk) {
        Serial.println("[GPRS] Transmission cycle due");

        simInit();
        simWaitBoot();

        // Check network registration
        if (!simNetworkRegistered()) {
            Serial.println("[GPRS] Not registered on network - skipping");
            reading.fault_flags |= FAULT_GPRS_FAIL;
            storageLogEvent(currentTime, "GPRS: Not registered on network");
        } else {
            Serial.printf("[GPRS] Signal quality: %d/31\n", simSignalQuality());

            if (gprsConnect()) {
                // First boot: attempt NTP sync to set RTC correctly
                if (!rtcSynced) {
                    Serial.println("[NTP] First-boot NTP sync...");
                    if (rtcSyncFromNTP()) {
                        rtcSynced = true;
                        currentTime = rtcGetUnixTime();    // Update with accurate time
                        storageLogEvent(currentTime, "NTP sync successful");
                    }
                }

                // static because 12KB exceeds the default 8KB task stack.
                // Sized for 96 records * ~120 bytes. Truncates rather than
                // overflows if that estimate is low.
                static char jsonBuf[12288];
                int recordCount = storageReadBufferAsJSON(jsonBuf,
                                                          sizeof(jsonBuf),
                                                          BATCH_UPLOAD_MAX);

                if (recordCount > 0) {
                    Serial.printf("[GPRS] Uploading %d records...\n", recordCount);

                    if (httpPost(jsonBuf)) {
                        storageArchiveBuffer();
                        lastTransmitTime = currentTime;
                        Serial.println("[GPRS] Upload complete");
                    } else {
                        reading.fault_flags |= FAULT_GPRS_FAIL;
                        Serial.println("[GPRS] Upload failed - data stays in buffer");
                        storageLogEvent(currentTime, "HTTP POST failed - buffer retained");

                        // Trim buffer if it has grown very large (>BATCH_UPLOAD_MAX)
                        if (storageBufferCount() > BATCH_UPLOAD_MAX) {
                            storageTrimBuffer(BATCH_UPLOAD_MAX);
                        }
                    }
                }

                gprsDisconnect();
                simSleep();     // Return SIM800L to ~0.7mA before deep sleep
            } else {
                reading.fault_flags |= FAULT_GPRS_FAIL;
                storageLogEvent(currentTime, "GPRS bearer failed to open");
            }
        }
    }

    // 10. LOG FAULT EVENTS
    if (reading.fault_flags != FAULT_NONE) {
        char faultMsg[64];
        snprintf(faultMsg, sizeof(faultMsg), "Faults: 0x%02X", reading.fault_flags);
        storageLogEvent(currentTime, faultMsg);
    }

    if (reading.tank_dry) {
        storageLogEvent(currentTime, "Tank dry detected");
    }

    // 11. SLEEP
    Serial.println("[SLEEP] Entering deep sleep");
    goToSleep(samplingFastMode);
}

// LOOP. Never reached (ESP32 restarts via deep sleep)
void loop() {
    // Intentionally empty
}

// DEEP SLEEP
void goToSleep(bool fastMode) {
    int sleepMinutes = fastMode ?
                       SAMPLE_INTERVAL_ANOMALY_MIN :
                       SAMPLE_INTERVAL_NORMAL_MIN;

    digitalWrite(PIN_STATUS_LED, LOW);
    digitalWrite(PIN_SENSOR_POWER, LOW);   // Belt-and-suspenders: ensure sensors off

    Serial.printf("[SLEEP] Sleeping for %d minutes\n", sleepMinutes);
    Serial.flush();

    uint64_t sleepUs = (uint64_t)sleepMinutes * uS_PER_MINUTE;
    esp_sleep_enable_timer_wakeup(sleepUs);
    esp_deep_sleep_start();
}
