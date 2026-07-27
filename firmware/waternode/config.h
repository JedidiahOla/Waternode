#ifndef CONFIG_H
#define CONFIG_H

// WATERNODE FIRMWARE - CONFIGURATION
// Low-Cost Water Quality Monitoring System for Rural Nepal
// Dublin City University - Final Year Project

// NODE IDENTITY
#define NODE_ID             "WN001"
#define FIRMWARE_VERSION    "1.0.0"

// GPIO PIN ASSIGNMENTS
// All pins verified conflict-free for ESP32-WROOM-32

// Sensors
#define PIN_DS18B20         4       // DS18B20 OneWire data
// GPIO26 is available, reserved for future pH sensor or other use
#define PIN_BATTERY_ADC     36      // Battery voltage divider (input-only ADC)

// Turbidity and TDS are read via an external ADS1115 16-bit ADC (I2C), not the
// ESP32's internal ADC, the internal ADC has known nonlinearity above ~3.1V
// and below ~0.15V. The ADS1115 shares the I2C bus with the DS3231 RTC below.
#define ADS1115_I2C_ADDR      0x48   // ADDR pin tied to GND
#define ADS1115_CHANNEL_TURB  0      // A0: turbidity, via 100k/100k divider
#define ADS1115_CHANNEL_TDS   1      // A1: TDS, direct (max ~2.3V, within range)

// SD Card (SPI bus)
#define PIN_SD_MOSI         23
#define PIN_SD_MISO         19
#define PIN_SD_CLK          18
#define PIN_SD_CS           5

// DS3231 RTC (I2C bus)
#define PIN_I2C_SDA         21
#define PIN_I2C_SCL         22

// SIM800L (UART2)
#define PIN_SIM_TX          17      // ESP32 TX -> SIM800L RX
#define PIN_SIM_RX          16      // ESP32 RX <- SIM800L TX
#define PIN_SIM_PWRKEY      13      // SIM800L PWRKEY, pulse LOW to power on/reset

// Sensor power control via MOSFET gates
// All sensors switched off during deep sleep to eliminate quiescent draw
#define PIN_SENSOR_POWER    27      // Controls MOSFET gate for all sensors

// Status LED (optional, for lab debugging)
#define PIN_STATUS_LED      2       // Onboard LED

// SAMPLING CONFIGURATION
#define SAMPLE_INTERVAL_NORMAL_MIN      15      // Minutes between samples (normal)
#define SAMPLE_INTERVAL_ANOMALY_MIN     5       // Minutes between samples (anomaly active)
#define TRANSMIT_INTERVAL_HOURS         4       // Hours between GPRS uploads
#define SAMPLES_PER_READING             10      // Raw samples per sensor per cycle
#define SAMPLES_DISCARD_EACH_END        2       // Outliers to discard high and low
                                                // Remaining averaged: 10 - (2*2) = 6
#define SENSOR_WARMUP_MS                2000    // Sensor power-on stabilisation
#define DS18B20_RESOLUTION              9       // Bits (9=94ms, less drain than 12=750ms)

// ANOMALY MODE MANAGEMENT
// Once confirmed anomaly detected, stay in fast-sample mode for this duration
// before reverting to normal, even if readings normalise
#define ANOMALY_RECOVERY_MINUTES        60      // Stay in fast mode for 1hr after clear

// LEARNING MODE
// System collects baseline data but suppresses CUSUM alerts (handled on backend)
// Local absolute-limit fallback remains active during learning
#define LEARNING_MODE_DURATION_DAYS     14

// SEASONAL BASELINE PARAMETERS
// All four seasons are WHO/literature defaults for the hypothetical
// deployment, not measurements from a real site. Recalibrate from local data
// before deploying.
//
// Note: backend config.py holds the same table but with the Dry-season row
// replaced by real Dublin bench measurements. The two tables differing on
// Dry season is intentional.
// Season indices: 0=Dry(Nov-Apr), 1=PreMonsoon(May-Jun),
//                 2=Monsoon(Jul-Sep),  3=PostMonsoon(Oct)
const float TURB_MEAN[4]  = {3.0f,   8.0f,  25.0f, 10.0f};  // NTU
const float TURB_STD[4]   = {1.5f,   5.0f,  15.0f,  6.0f};  // NTU
const float TDS_MEAN[4]   = {180.0f, 150.0f, 120.0f, 160.0f}; // mg/L
// Note: TDS decreases during monsoon, rainfall dilutes dissolved solids
const float TDS_STD[4]    = {40.0f,  50.0f,  60.0f,  45.0f};  // mg/L
const float TEMP_MEAN[4]  = {14.0f,  19.0f,  22.0f,  17.0f};  // C
const float TEMP_STD[4]   = {3.0f,   3.0f,   2.0f,   3.0f};   // C

// LOCAL ALERT THRESHOLDS (ESP32-side, offline-capable)
// Alert fires when reading exceeds (seasonal_mean * LOCAL_THRESHOLD_MULTIPLIER)
// This is separate from backend CUSUM, acts as immediate SMS failsafe
#define LOCAL_THRESHOLD_MULTIPLIER      3.0f    // 3x seasonal mean

// WHO ABSOLUTE LIMITS. Active during learning mode as fallback
// Source: WHO Guidelines for Drinking-Water Quality, 4th ed., 2022
#define WHO_TURBIDITY_LIMIT_NTU         4.0f    // WHO guideline value (4th ed., 2022)
#define WHO_TDS_LIMIT_MGL               900.0f  // Above palatability threshold

// TURBIDITY SENSOR CALIBRATION. DFRobot SEN0189
// Powered at 5V from the MT3608. Signal passes through a 100k ohm/100k ohm divider
// into ADS1115 A0, so multiply by TURB_DIVIDER_RATIO before applying the
// formula below.
//
// DFRobot empirical formula, valid at 5V supply:
//   NTU = TURB_A*V^2 + TURB_B*V + TURB_C   (V = sensor voltage)
// Output is inverse: clear water ~4.3V, 3000 NTU ~2.5V.
// Not yet verified against bentonite standards on the built unit.
#define TURB_A              -1120.4f    // DFRobot quadratic coefficient (5V supply)
#define TURB_B               5742.3f   // DFRobot linear coefficient
#define TURB_C              -4353.8f   // DFRobot offset
#define TURB_DIVIDER_RATIO   2.0f      // Voltage divider factor (halved at ADC pin)
#define TURB_V_CLEAR         4.30f     // Expected sensor output in clear water at 5V supply
#define TURB_V_MIN           0.80f     // Below this (actual sensor V) -> disconnected/fouled

// TDS SENSOR CALIBRATION. DFRobot SEN0244
// Multiplier on the conversion polynomial in sensors.cpp. 1.0 = uncalibrated,
// so absolute mg/L isn't trustworthy yet. To calibrate: submerge in a known
// standard (e.g. 342 ppm NaCl) and set TDS_KVALUE = known_ppm / reported_ppm.
#define TDS_KVALUE           1.0f

// BATTERY VOLTAGE MONITORING
// 100k ohm / 100k ohm voltage divider on PIN_BATTERY_ADC
// Divides battery voltage by 2: 4.2V -> 2.1V, 3.0V -> 1.5V
// ADC calibration correction applied in firmware (esp-idf adc_chars)
#define BATTERY_DIVIDER_RATIO       2.0f    // Resistor divider ratio
#define BATTERY_LOW_MV              3300    // Low battery warning threshold (mV)
#define BATTERY_CRITICAL_MV         3000    // Critical: stop transmitting, save energy
#define BATTERY_FULL_MV             4200    // Reference for percentage calculation

// CONNECTIVITY. SIM800L / GSM
#define GSM_BAUD_RATE       9600
#define APN                 "ntc.com.np"    // Nepal Telecom APN
#define APN_USER            ""              // NTC requires no credentials
#define APN_PASS            ""

// Alert SMS recipients, up to 3 numbers
// In deployment: water committee chair, health post, ward office
// In lab/testing: developer's number
#define SMS_NUMBER_1        "+353XXXXXXXXX"   // Replace with test number
#define SMS_NUMBER_2        ""                // Leave empty if unused
#define SMS_NUMBER_3        ""                // Leave empty if unused

// SMS suppression: minimum gap between alerts for same parameter (minutes)
#define SMS_SUPPRESSION_MINUTES     60

// AT-command handshake tuning
#define AT_COMMAND_TIMEOUT_MS       1000    // Default wait for a plain "OK"
#define SIM_BOOT_RETRY_ATTEMPTS     3       // AT probes before a PWRKEY power-cycle
#define GPRS_BEARER_TIMEOUT_MS      10000   // AT+SAPBR=1,1 (open bearer) timeout
#define NTP_SYNC_TIMEOUT_MS         10000   // AT+CNTP / +CCLK? round trip timeout

// BACKEND SERVER
// Development: ngrok tunnel to local Flask instance
// Change to PythonAnywhere URL when deployed
#define SERVER_URL          "http://YOUR-NGROK-URL.ngrok.io"
#define API_ENDPOINT        "/api/readings"
#define API_KEY             "wn-dev-key-2026"   // Simple shared secret
// TODO: Replace with proper token auth for production deployment

// HTTP response timeout
#define HTTP_TIMEOUT_MS     15000

// Maximum records to upload in a single batch POST
#define BATCH_UPLOAD_MAX    96      // Max 4hrs worth at 15-min intervals

// SD CARD FILE NAMES
#define SD_BUFFER_FILE      "/buffer.csv"
#define SD_ARCHIVE_FILE     "/archive.csv"
#define SD_LOG_FILE         "/system.log"

// CSV header row, must match exactly the order fields are written in storage.cpp
#define CSV_HEADER "timestamp,node_id,turbidity_ntu,turbidity_raw," \
                   "tds_mgl,temperature_c," \
                   "battery_mv,alert_level,fault_flags,season_index\n"

// FAULT FLAG BITMASK
// Individual bits in the fault_flags byte stored per reading
// Allows multiple simultaneous faults to be recorded
#define FAULT_NONE              0x00
#define FAULT_TURB_SENSOR       0x01    // Turbidity sensor implausible/offline
#define FAULT_TDS_SENSOR        0x02    // TDS sensor implausible/offline
#define FAULT_TEMP_SENSOR       0x04    // Temperature sensor not responding
#define FAULT_SD_WRITE          0x08    // SD card write failure
#define FAULT_GPRS_FAIL         0x10    // GPRS connection failed
#define FAULT_LOW_BATTERY       0x20    // Battery below warning threshold
#define FAULT_TANK_DRY          0x40    // All sensors suggest tank empty/dry
#define FAULT_RTC_FAIL          0x80    // DS3231 not responding

// Sensor plausibility bounds, readings outside these are flagged as faults
#define TURB_MIN_PLAUSIBLE      0.0f    // NTU
#define TURB_MAX_PLAUSIBLE      1000.0f // NTU
#define TDS_MIN_PLAUSIBLE       0.0f    // mg/L
#define TDS_MAX_PLAUSIBLE       2000.0f // mg/L
#define TEMP_MIN_PLAUSIBLE      0.0f    // C (Nepal mid-hills, never below freezing)
#define TEMP_MAX_PLAUSIBLE      40.0f   // C

// Tank dry detection: if turbidity is implausibly flat AND TDS near zero
#define TANK_DRY_TDS_THRESHOLD  5.0f    // mg/L. Very low TDS suggests air.

// ALERT LEVELS (matching backend CUSUM levels)
// Level generated locally is conservative, backend may escalate
#define ALERT_NONE          0
#define ALERT_WATCH         1   // Single parameter slightly elevated
#define ALERT_WARNING       2   // Parameter significantly elevated, 1 reading
#define ALERT_CONFIRMED     3   // Elevated for 3+ consecutive readings -> triggers SMS

// Persistence: number of consecutive anomalous readings before CONFIRMED alert
#define ALERT_PERSISTENCE   3

// DEEP SLEEP
// ESP32 deep sleep duration calculated at runtime based on sampling mode
// Stored in RTC memory to survive sleep cycles
#define uS_PER_MINUTE       60000000ULL    // Microseconds per minute

// NTP CONFIGURATION
// Used for initial DS3231 time sync over GPRS
#define NTP_SERVER          "pool.ntp.org"
#define NTP_GMT_OFFSET_SEC  20700    // Nepal Standard Time = UTC+5:45

#endif // CONFIG_H
