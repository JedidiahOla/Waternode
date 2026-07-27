#include "sensors.h"
#include "config.h"
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_ADS1X15.h>
#include <algorithm>

// Early bring-up used a generic unbranded turbidity board and a Sarini-brand
// TDS clone, both already on hand (see hardware-bringup/temp_tds_test.ino).
// Both were swapped for genuine DFRobot SEN0189/SEN0244 units before final
// calibration, for datasheet reliability - this file reflects that final
// hardware.

// Sensor objects
static OneWire oneWire(PIN_DS18B20);
static DallasTemperature ds18b20(&oneWire);

static Adafruit_ADS1115 ads;
static bool adsAvailable = false;


void sensorsInit() {
    // MOSFET gate - start with sensors off
    pinMode(PIN_SENSOR_POWER, OUTPUT);
    digitalWrite(PIN_SENSOR_POWER, LOW);

    // Battery ADC on internal pin
    analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

    // Must run before ads.begin(), or the library uses the core's default
    // pins. rtcInit() usually did this already; calling twice is safe and
    // keeps this module usable standalone from a calibration sketch.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    // ADS1115 - shares I2C with the RTC
    if (ads.begin(ADS1115_I2C_ADDR)) {
        adsAvailable = true;
        ads.setGain(GAIN_ONE);      // +/-4.096V range
        Serial.println("[SENSORS] ADS1115 found");
    } else {
        adsAvailable = false;
        Serial.println("[SENSORS] ADS1115 NOT FOUND");
    }

    // DS18B20
    ds18b20.begin();
    ds18b20.setResolution(DS18B20_RESOLUTION);
    ds18b20.setWaitForConversion(false);
}

void sensorsOn() {
    digitalWrite(PIN_SENSOR_POWER, HIGH);
    delay(SENSOR_WARMUP_MS);
}

void sensorsOff() {
    digitalWrite(PIN_SENSOR_POWER, LOW);
}


// Sort samples, throw away outliers from each end, average the rest
static float filteredAverage(float* samples, int count, int discard) {
    std::sort(samples, samples + count);
    float sum = 0.0f;
    int validCount = count - (2 * discard);
    if (validCount <= 0) return 0.0f;
    for (int i = discard; i < count - discard; i++) {
        sum += samples[i];
    }
    return sum / validCount;
}

// Same thing but for int arrays (used by battery reading)
static float filteredAverageInt(int* samples, int count, int discard) {
    std::sort(samples, samples + count);
    float sum = 0.0f;
    int validCount = count - (2 * discard);
    if (validCount <= 0) return 0.0f;
    for (int i = discard; i < count - discard; i++) {
        sum += samples[i];
    }
    return sum / validCount;
}


// --- Turbidity ---
// DFRobot SEN0189, powered from 5V (MT3608 boost output)
// Signal goes through 100k/100k divider before ADS1115 A0
// so we read half the actual voltage and multiply back up
// Output is inverse: higher voltage = clearer water

int readTurbidityRaw() {
    if (!adsAvailable) return 0;

    float samples[SAMPLES_PER_READING];
    for (int i = 0; i < SAMPLES_PER_READING; i++) {
        int16_t raw = ads.readADC_SingleEnded(ADS1115_CHANNEL_TURB);
        samples[i] = (float)raw;
        delay(10);
    }
    return (int)filteredAverage(samples, SAMPLES_PER_READING,
                                SAMPLES_DISCARD_EACH_END);
}

// Convert raw ADS1115 count to NTU
// Using this separately so readAllSensors() can store both raw and NTU
// from the same set of samples
float readTurbidityFromRaw(int raw) {
    float v_adc = raw * (4.096f / 32767.0f);

    // Undo the voltage divider
    float voltage = v_adc * TURB_DIVIDER_RATIO;

    // Way too low = sensor disconnected
    if (voltage < TURB_V_MIN) {
        return TURB_MAX_PLAUSIBLE + 1.0f;   // will trigger fault flag
    }

    // DFRobot quadratic formula
    float ntu = TURB_A * voltage * voltage
              + TURB_B * voltage
              + TURB_C;

    // Clamp - formula can go slightly negative in very clear water
    if (ntu < TURB_MIN_PLAUSIBLE) ntu = TURB_MIN_PLAUSIBLE;
    if (ntu > TURB_MAX_PLAUSIBLE) ntu = TURB_MAX_PLAUSIBLE;

    return ntu;
}

float readTurbidity() {
    return readTurbidityFromRaw(readTurbidityRaw());
}


// --- TDS ---
// DFRobot SEN0244, signal goes directly to ADS1115 A1
// Formula from DFRobot wiki with temperature compensation

float readTDS(float temperature_c) {
    if (!adsAvailable) return 0.0f;

    float voltageSamples[SAMPLES_PER_READING];

    for (int i = 0; i < SAMPLES_PER_READING; i++) {
        int16_t raw = ads.readADC_SingleEnded(ADS1115_CHANNEL_TDS);
        voltageSamples[i] = raw * (4.096f / 32767.0f);
        delay(10);
    }

    float voltage = filteredAverage(voltageSamples, SAMPLES_PER_READING,
                                    SAMPLES_DISCARD_EACH_END);

    // Temperature compensation (reference 25C, 0.02 per degree)
    float compensationCoeff = 1.0f + 0.02f * (temperature_c - 25.0f);
    float compensatedVoltage = voltage / compensationCoeff;

    // Conversion formula with calibration kValue
    float tds = (133.42f * compensatedVoltage * compensatedVoltage * compensatedVoltage
               - 255.86f * compensatedVoltage * compensatedVoltage
               + 857.39f * compensatedVoltage) * 0.5f * TDS_KVALUE;

    if (tds < TDS_MIN_PLAUSIBLE) tds = TDS_MIN_PLAUSIBLE;
    if (tds > TDS_MAX_PLAUSIBLE) tds = TDS_MAX_PLAUSIBLE;

    return tds;
}


// --- Temperature ---
// DS18B20, non-blocking read
// Rejects known bogus values:
//   85.0 = power-on reset default (looks valid but isn't)
//   0.0  = data line probably shorted to ground
//  -127  = sensor not found / CRC error

float readTemperature() {
    ds18b20.requestTemperatures();
    delay(100);     // 94ms min for 9-bit + some margin

    float tempC = ds18b20.getTempCByIndex(0);

    if (tempC == DEVICE_DISCONNECTED_C || tempC < -50.0f) {
        return -999.0f;
    }

    if (tempC == 85.0f) {
        Serial.println("[SENSORS] DS18B20: 85C rejected (power-on reset)");
        return -999.0f;
    }

    // 0C is technically possible but not plausible for Nepal mid-hills water
    if (tempC == 0.0f) {
        Serial.println("[SENSORS] DS18B20: 0C rejected (data line short?)");
        return -999.0f;
    }

    return tempC;
}


// --- Battery ---
// 100k/100k divider on GPIO36 (internal ADC)
// Not very accurate but good enough for "is the battery low" checks

int readBatteryMv() {
    int samples[SAMPLES_PER_READING];
    for (int i = 0; i < SAMPLES_PER_READING; i++) {
        samples[i] = analogRead(PIN_BATTERY_ADC);
        delay(5);
    }
    float avgRaw = filteredAverageInt(samples, SAMPLES_PER_READING,
                                      SAMPLES_DISCARD_EACH_END);

    float dividedMv = avgRaw * 3300.0f / 4095.0f;
    float batteryMv = dividedMv * BATTERY_DIVIDER_RATIO;

    // Empirical correction for ESP32 ADC nonlinearity
    // TODO: should really use esp_adc_cal_characterize() for this
    batteryMv *= 1.07f;

    return (int)batteryMv;
}


// Read everything, check for faults, return the struct
// Caller is responsible for calling sensorsOn() / sensorsOff()

SensorReading readAllSensors() {
    SensorReading reading;
    reading.fault_flags = FAULT_NONE;
    reading.tank_dry = false;

    // Temperature first because TDS needs it for compensation
    reading.temperature_c = readTemperature();
    if (reading.temperature_c < -900.0f) {
        reading.temperature_c = 20.0f;     // fallback so TDS calc still works
        reading.fault_flags |= FAULT_TEMP_SENSOR;
    } else if (reading.temperature_c < TEMP_MIN_PLAUSIBLE ||
               reading.temperature_c > TEMP_MAX_PLAUSIBLE) {
        reading.fault_flags |= FAULT_TEMP_SENSOR;
        reading.temperature_c = 20.0f;
    }

    // TDS
    reading.tds_mgl = readTDS(reading.temperature_c);
    if (!adsAvailable) {
        reading.fault_flags |= FAULT_TDS_SENSOR;
    } else if (reading.tds_mgl < TDS_MIN_PLAUSIBLE ||
               reading.tds_mgl > TDS_MAX_PLAUSIBLE) {
        reading.fault_flags |= FAULT_TDS_SENSOR;
    }

    // Turbidity - read raw first, then convert, so both come from same samples
    reading.turbidity_raw = readTurbidityRaw();
    reading.turbidity_ntu = readTurbidityFromRaw(reading.turbidity_raw);
    if (!adsAvailable) {
        reading.fault_flags |= FAULT_TURB_SENSOR;
    } else if (reading.turbidity_ntu < TURB_MIN_PLAUSIBLE ||
               reading.turbidity_ntu > TURB_MAX_PLAUSIBLE) {
        reading.fault_flags |= FAULT_TURB_SENSOR;
    }

    // Battery
    reading.battery_mv = readBatteryMv();
    if (reading.battery_mv < BATTERY_LOW_MV) {
        reading.fault_flags |= FAULT_LOW_BATTERY;
    }

    // Tank dry detection - if turbidity near 0 AND TDS near 0,
    // sensors are probably in air not water
    if (reading.tds_mgl < TANK_DRY_TDS_THRESHOLD &&
        reading.turbidity_ntu < 2.0f) {
        reading.tank_dry = true;
        reading.fault_flags |= FAULT_TANK_DRY;
    }

    return reading;
}


int getSeasonIndex(int month) {
    if (month >= 11 || month <= 4)  return 0;   // Dry: Nov-Apr
    if (month >= 5  && month <= 6)  return 1;   // Pre-monsoon: May-Jun
    if (month >= 7  && month <= 9)  return 2;   // Monsoon: Jul-Sep
    return 3;                                    // Post-monsoon: Oct
}
