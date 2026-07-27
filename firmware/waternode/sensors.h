#ifndef SENSORS_H
#define SENSORS_H

#include <Arduino.h>

// SENSORS MODULE
// Handles all sensor initialisation and reading
// Sensors powered via MOSFET, call sensorsOn() before reading,
// sensorsOff() when done to eliminate quiescent drain during sleep

// Struct to hold one complete sensor reading
struct SensorReading {
    float turbidity_ntu;    // Converted from ADC via calibration curve
    int   turbidity_raw;    // Raw ADC value, stored for post-calibration reprocessing
    float tds_mgl;          // Temperature-compensated TDS in mg/L
    float temperature_c;    // Water temperature in C
    int   battery_mv;       // Battery voltage in millivolts
    uint8_t fault_flags;    // Bitmask of any faults detected this reading
    bool  tank_dry;         // True if sensor suite suggests tank is empty
};

// Initialise sensor pins and OneWire bus
// Call once at startup (each wake cycle)
void sensorsInit();

// Power sensors on via MOSFET, allow warmup before reading
void sensorsOn();

// Power sensors off, eliminates all quiescent draw during sleep
void sensorsOff();

// Take a full reading from all sensors
// Internally takes SAMPLES_PER_READING samples, removes outliers, averages
// Returns populated SensorReading struct with fault_flags set for any issues
SensorReading readAllSensors();

// Individual sensor functions (exposed for testing/calibration mode)
int   readTurbidityRaw();              // Returns filtered average ADC count
float readTurbidityFromRaw(int raw);   // Converts a raw ADC count to NTU
float readTurbidity();                 // Convenience: readTurbidityFromRaw(readTurbidityRaw())
float readTDS(float temperature_c);    // Returns mg/L, temperature-compensated
float readTemperature();    // Returns C
int   readBatteryMv();      // Returns battery voltage in mV

// Get current season index based on month
// 0=Dry(Nov-Apr), 1=PreMonsoon(May-Jun), 2=Monsoon(Jul-Sep), 3=PostMonsoon(Oct)
int getSeasonIndex(int month);

#endif // SENSORS_H
