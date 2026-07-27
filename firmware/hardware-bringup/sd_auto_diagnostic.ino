// SD CARD AUTO-DIAGNOSTIC. ESP32
// Runs all checks automatically, no multimeter needed
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

SPIClass spi = SPIClass(VSPI);

// Try different CS pins in case wiring is swapped
int csPins[] = {5, 15, 4, 2, 13, 14, 27, 26, 25};
int numCsPins = 9;

// Try different SPI speeds
uint32_t speeds[] = {400000, 1000000, 4000000, 8000000};
int numSpeeds = 4;

bool trySD(int csPin, uint32_t speed) {
    SD.end();
    delay(100);
    spi.end();
    delay(100);
    spi.begin(18, 19, 23, csPin);
    delay(100);
    return SD.begin(csPin, spi, speed);
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("========================================");
    Serial.println("SD Card Auto-Diagnostic");
    Serial.println("========================================\n");

    // TEST 1: Check MISO pin state
    // If MISO is stuck LOW with module powered, card may not be inserted
    Serial.println("--- Test 1: MISO pin state ---");
    pinMode(19, INPUT);
    int misoState = digitalRead(19);
    Serial.printf("MISO (D19) reads: %s\n", misoState ? "HIGH" : "LOW");
    if (misoState == LOW) {
        Serial.println("  WARNING: MISO is LOW - possible issues:");
        Serial.println("  - Card not inserted");
        Serial.println("  - MISO wire not connected");
        Serial.println("  - Module not powered");
    } else {
        Serial.println("  OK - MISO is HIGH (module is responding on this line)");
    }

    // TEST 2: Try SD init on correct CS pin with different speeds
    Serial.println("\n--- Test 2: Try D5 (CS) at different speeds ---");
    for (int s = 0; s < numSpeeds; s++) {
        Serial.printf("  Trying CS=D5, speed=%luHz... ", speeds[s]);
        if (trySD(5, speeds[s])) {
            Serial.println("SUCCESS!");
            Serial.printf("  Card size: %lluMB\n", SD.totalBytes() / (1024 * 1024));
            printSuccess();
            return;
        } else {
            Serial.println("FAILED");
        }
        delay(200);
    }

    // TEST 3: Try different CS pins in case wires are swapped
    Serial.println("\n--- Test 3: Try alternate CS pins ---");
    Serial.println("  (In case CS wire is in wrong terminal)");
    for (int c = 0; c < numCsPins; c++) {
        if (csPins[c] == 5) continue;   // Already tried
        Serial.printf("  Trying CS=D%d at 400kHz... ", csPins[c]);
        pinMode(csPins[c], OUTPUT);
        if (trySD(csPins[c], 400000)) {
            Serial.println("SUCCESS!");
            Serial.printf("  *** CS wire is in D%d, not D5! ***\n", csPins[c]);
            Serial.printf("  Card size: %lluMB\n", SD.totalBytes() / (1024 * 1024));
            printSuccess();
            return;
        } else {
            Serial.println("FAILED");
        }
        delay(200);
    }

    // TEST 4: SPI loopback test
    // Connect MOSI to MISO temporarily to verify SPI bus works
    Serial.println("\n--- Test 4: SPI bus check ---");
    SD.end();
    spi.end();
    delay(100);
    spi.begin(18, 19, 23, 5);

    // Send a byte and see if SPI peripheral is working
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);  // Deselect SD
    spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    uint8_t sent = 0xAA;
    uint8_t received = spi.transfer(sent);
    spi.endTransaction();

    Serial.printf("  Sent 0x%02X, received 0x%02X\n", sent, received);
    if (received == 0xFF) {
        Serial.println("  SPI bus is working (0xFF = no device responding, normal)");
    } else if (received == sent) {
        Serial.println("  WARNING: Received same byte sent - MOSI/MISO may be shorted");
    } else {
        Serial.printf("  Received unexpected byte - possible noise or wiring issue\n");
    }

    // SUMMARY
    Serial.println("\n========================================");
    Serial.println("ALL ATTEMPTS FAILED");
    Serial.println("========================================");
    Serial.println("Diagnosis:");

    if (misoState == LOW) {
        Serial.println("  -> MISO not responding. Check:");
        Serial.println("    - Is the SD card inserted and clicked in?");
        Serial.println("    - Is the MISO wire in D19?");
        Serial.println("    - Is VCC getting 5V?");
    } else {
        Serial.println("  -> SPI lines seem connected but card not initialising.");
        Serial.println("    Most likely causes:");
        Serial.println("    1. SD card itself is incompatible (try a different card)");
        Serial.println("    2. Card not formatted as FAT32");
        Serial.println("    3. MOSI and MISO wires swapped");
        Serial.println("       (try swapping D19 and D23 wires)");
    }
}

void printSuccess() {
    // Quick write/read test
    Serial.println("\n--- Write/Read test ---");
    if (SD.exists("/test.csv")) SD.remove("/test.csv");

    File f = SD.open("/test.csv", FILE_WRITE);
    if (f) {
        f.println("timestamp,tds,temp");
        f.println("1711700000,185.00,21.0");
        f.close();
        Serial.println("  Write OK");
    }

    f = SD.open("/test.csv", FILE_READ);
    if (f) {
        Serial.println("  Read OK - contents:");
        while (f.available()) {
            Serial.write(f.read());
        }
        f.close();
    }

    Serial.println("\n========================================");
    Serial.println("SD CARD WORKING");
    Serial.println("========================================");
}

void loop() {
    delay(10000);
}
