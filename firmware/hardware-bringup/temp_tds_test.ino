// DS18B20 + TDS SENSOR BREADBOARD TEST. ESP32
// WaterNode Project. DCU FYP
//
// Wiring (using ESP32 Terminal Adapter):
//
//   DS18B20 (waterproof probe):
//     Red wire    -> 3V3 terminal
//     Black wire  -> GND terminal
//     Yellow wire -> D4 terminal
//     4.7k ohm resistor between 3V3 and D4 (pullup, mandatory)
//
//   Written during initial sensor bring-up, before switching to genuine
//   DFRobot parts - see waternode/sensors.cpp for the final implementation.
//   Sarini TDS Sensor board (pin order left to right: -/+/A):
//     - pin -> GND terminal
//     + pin -> 3V3 terminal
//     A pin -> D34 terminal
//     Probe plugged into board's XH2.54-2P connector
//
// Expected output:
//   Temperature: ~18-25C (room temp, or water temp if submerged)
//   TDS in air:  ~0 ppm (probe not in water)
//   TDS in tap water: ~100-400 ppm (varies by location)
//
// Red flags:
//   Temperature 85.0C   -> DS18B20 power-on reset (check data line)
//   Temperature 0.0C    -> Data line shorted to GND
//   Temperature -127.0C -> Sensor not found
//   TDS voltage stuck at 0.0V -> Check wiring or probe connection
//   TDS voltage stuck at 3.3V -> Possible short on signal line

#include <OneWire.h>
#include <DallasTemperature.h>

// --- Pin assignments ---
#define PIN_DS18B20     4
#define PIN_TDS_ANALOG  34

// --- DS18B20 setup ---
OneWire oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("========================================");
    Serial.println("DS18B20 + TDS Sensor Test");
    Serial.println("========================================");

    // --- DS18B20 init ---
    tempSensor.begin();
    int deviceCount = tempSensor.getDeviceCount();
    Serial.printf("DS18B20 devices found: %d\n", deviceCount);

    if (deviceCount == 0) {
        Serial.println("ERROR: No DS18B20 found!");
        Serial.println("  Check: 4.7k pullup between 3V3 and D4?");
        Serial.println("  Check: Yellow wire on D4?");
        Serial.println("  Check: Red=3V3, Black=GND?");
        // Don't halt, still test TDS even if temp sensor missing
    } else {
        tempSensor.setResolution(9);
        Serial.println("DS18B20 resolution set to 9-bit");

        DeviceAddress addr;
        if (tempSensor.getAddress(addr, 0)) {
            Serial.print("DS18B20 address: ");
            for (int i = 0; i < 8; i++) {
                if (addr[i] < 16) Serial.print("0");
                Serial.print(addr[i], HEX);
            }
            Serial.println();
        }
    }

    // --- TDS pin setup ---
    analogSetPinAttenuation(PIN_TDS_ANALOG, ADC_11db);
    Serial.println("TDS analog pin (GPIO34) configured");

    Serial.println("----------------------------------------");
    Serial.println("Reading every 2 seconds...\n");
}

void loop() {
    // 1. READ TEMPERATURE
    float tempC = -999.0f;
    bool tempValid = false;

    if (tempSensor.getDeviceCount() > 0) {
        tempSensor.requestTemperatures();
        delay(100);     // 94ms for 9-bit + margin
        tempC = tempSensor.getTempCByIndex(0);
    }

    // Check for known bad values
    if (tempC <= -126.0f) {
        Serial.print("TEMP: SENSOR NOT FOUND  |  ");
        tempC = 25.0f;     // Default for TDS compensation
    } else if (tempC == 85.0f) {
        Serial.print("TEMP: 85.0C [RESET - check wiring]  |  ");
        tempC = 25.0f;
    } else if (tempC == 0.0f) {
        Serial.print("TEMP: 0.0C [POSSIBLE SHORT]  |  ");
        tempC = 25.0f;
    } else {
        Serial.printf("TEMP: %.1fC  |  ", tempC);
        tempValid = true;
    }

    // 2. READ TDS
    // Take 10 samples, sort, discard 2 from each end, average middle 6
    float samples[10];
    for (int i = 0; i < 10; i++) {
        samples[i] = (float)analogRead(PIN_TDS_ANALOG);
        delay(10);
    }

    // Bubble sort for outlier removal
    for (int i = 0; i < 9; i++) {
        for (int j = i + 1; j < 10; j++) {
            if (samples[i] > samples[j]) {
                float tmp = samples[i];
                samples[i] = samples[j];
                samples[j] = tmp;
            }
        }
    }

    // Average middle 6
    float sum = 0.0f;
    for (int i = 2; i < 8; i++) {
        sum += samples[i];
    }
    float avgRaw = sum / 6.0f;

    // Convert 12-bit ADC to voltage
    float voltage = avgRaw * 3.3f / 4095.0f;

    // Temperature compensation (reference 25C)
    float compCoeff = 1.0f + 0.02f * (tempC - 25.0f);
    float compVoltage = voltage / compCoeff;

    // TDS conversion formula (from Sarini/DFRobot datasheet)
    // kValue not applied, calibrate later with known solution
    float tdsValue = (133.42f * compVoltage * compVoltage * compVoltage
                    - 255.86f * compVoltage * compVoltage
                    + 857.39f * compVoltage) * 0.5f;

    if (tdsValue < 0.0f) tdsValue = 0.0f;

    Serial.printf("TDS: %.0f ppm (%.3fV raw, %.3fV comp, ADC %.0f)\n",
                  tdsValue, voltage, compVoltage, avgRaw);

    delay(2000);
}
