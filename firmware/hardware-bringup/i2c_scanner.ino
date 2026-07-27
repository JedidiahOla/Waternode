// I2C SCANNER. ESP32
// WaterNode Project. DCU FYP
//
// Wiring (using ESP32 Terminal Adapter):
//   DS3231 and ADS1115 share the I2C bus:
//     Both SDA pins -> D21 terminal
//     Both SCL pins -> D22 terminal
//     Both VCC pins -> 3V3 terminal
//     Both GND pins -> GND terminal
//     ADS1115 ADDR  -> GND terminal (sets address to 0x48)
//
// Expected output:
//   0x48. ADS1115 ADC
//   0x57. DS3231 EEPROM (some modules include AT24C32)
//   0x68. DS3231 RTC
//
// If 0x48 missing: check ADS1115 ADDR pin is connected to GND
// If 0x68 missing: check DS3231 wiring and CR2032 battery

#include <Wire.h>

#define PIN_SDA 21
#define PIN_SCL 22

void setup() {
    Serial.begin(115200);
    delay(1000);

    Wire.begin(PIN_SDA, PIN_SCL);

    Serial.println("========================================");
    Serial.println("I2C Bus Scanner");
    Serial.println("========================================");
    Serial.println("Scanning...\n");

    int devicesFound = 0;

    for (byte addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        byte error = Wire.endTransmission();

        if (error == 0) {
            Serial.printf("  FOUND device at 0x%02X", addr);

            // Identify known devices
            if (addr == 0x48) Serial.print("  <- ADS1115 ADC");
            if (addr == 0x57) Serial.print("  <- DS3231 EEPROM (AT24C32)");
            if (addr == 0x68) Serial.print("  <- DS3231 RTC");

            Serial.println();
            devicesFound++;
        }
    }

    Serial.printf("\nTotal devices found: %d\n", devicesFound);

    if (devicesFound == 0) {
        Serial.println("ERROR: No I2C devices found!");
        Serial.println("  Check: SDA wire in D21?");
        Serial.println("  Check: SCL wire in D22?");
        Serial.println("  Check: VCC in 3V3, GND in GND?");
    }

    Serial.println("========================================");
    Serial.println("Scan complete. Reset ESP32 to scan again.");
}

void loop() {
    // Nothing, scan runs once in setup
    delay(10000);
}
