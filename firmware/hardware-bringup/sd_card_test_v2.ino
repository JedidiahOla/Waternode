// SD CARD TEST v2. ESP32
// Alternative SPI initialisation approach
//
// Wiring:
//   GND  -> GND
//   VCC  -> VIN (5V)
//   MISO -> D19
//   MOSI -> D23
//   SCK  -> D18
//   CS   -> D5

#include <SPI.h>
#include <SD.h>

#define PIN_SD_CS   5

// Use explicit VSPI bus
SPIClass spi = SPIClass(VSPI);

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("========================================");
    Serial.println("SD Card Test v2");
    Serial.println("========================================");

    // Start SPI on VSPI bus with default pins (18, 19, 23)
    spi.begin(18, 19, 23, PIN_SD_CS);

    Serial.println("Attempting SD init...");

    if (!SD.begin(PIN_SD_CS, spi, 4000000)) {    // 4MHz, slower for reliability
        Serial.println("FAILED at 4MHz.");
        Serial.println("Trying at 1MHz...");

        SD.end();
        delay(500);

        if (!SD.begin(PIN_SD_CS, spi, 1000000)) {    // 1MHz, very conservative
            Serial.println("FAILED at 1MHz too.");
            Serial.println();
            Serial.println("Possible causes:");
            Serial.println("  1. Card not inserted / not clicked in");
            Serial.println("  2. Card not FAT32 formatted");
            Serial.println("  3. Bad contact on a wire");
            Serial.println("  4. Faulty card");
            Serial.println();
            Serial.println("Try: remove card, reinsert firmly until click");
            Serial.println("Try: different SD card if available");
            while (true) { delay(1000); }
        }
    }

    Serial.println("SD card found!");
    Serial.printf("Card size: %lluMB\n", SD.totalBytes() / (1024 * 1024));

    // Write test
    if (SD.exists("/test.csv")) SD.remove("/test.csv");

    File f = SD.open("/test.csv", FILE_WRITE);
    if (f) {
        f.println("timestamp,node_id,tds,temp");
        f.println("1711700000,WN001,185.00,21.0");
        f.close();
        Serial.println("Write OK");
    } else {
        Serial.println("Write FAILED");
    }

    // Read test
    f = SD.open("/test.csv", FILE_READ);
    if (f) {
        Serial.println("Contents:");
        while (f.available()) Serial.write(f.read());
        f.close();
    }

    Serial.println("\nSD test complete.");
}

void loop() {
    delay(10000);
}
