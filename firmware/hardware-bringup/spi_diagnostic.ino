// SPI PIN DIAGNOSTIC. ESP32
// Tests if each SPI pin is actually reaching the SD module
//
// What this does:
//   Toggles each SPI pin HIGH/LOW every 2 seconds
//   Use multimeter on the SD MODULE side to confirm voltage changes
//
// If a pin doesn't toggle on the module -> broken wire or bad connection
// If all pins toggle but SD still fails -> card or module problem

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("========================================");
    Serial.println("SPI Pin Diagnostic");
    Serial.println("========================================");

    // Set all SPI pins as outputs for testing
    pinMode(5, OUTPUT);     // CS
    pinMode(18, OUTPUT);    // SCK
    pinMode(23, OUTPUT);    // MOSI
    // D19 (MISO) is an input from the SD module, we test it differently

    // Start all LOW
    digitalWrite(5, LOW);
    digitalWrite(18, LOW);
    digitalWrite(23, LOW);

    Serial.println("All pins LOW. Measure each on the SD module.");
    Serial.println("They should all read ~0V.");
    Serial.println();
    delay(5000);

    // Test CS (D5)
    Serial.println("--- Testing CS (D5) ---");
    Serial.println("D5 going HIGH...");
    digitalWrite(5, HIGH);
    Serial.println("Measure CS on module - should read ~3.3V");
    delay(5000);
    digitalWrite(5, LOW);
    Serial.println("D5 back to LOW - should read ~0V");
    delay(3000);

    // Test SCK (D18)
    Serial.println("\n--- Testing SCK (D18) ---");
    Serial.println("D18 going HIGH...");
    digitalWrite(18, HIGH);
    Serial.println("Measure SCK on module - should read ~3.3V");
    delay(5000);
    digitalWrite(18, LOW);
    Serial.println("D18 back to LOW - should read ~0V");
    delay(3000);

    // Test MOSI (D23)
    Serial.println("\n--- Testing MOSI (D23) ---");
    Serial.println("D23 going HIGH...");
    digitalWrite(23, HIGH);
    Serial.println("Measure MOSI on module - should read ~3.3V");
    delay(5000);
    digitalWrite(23, LOW);
    Serial.println("D23 back to LOW - should read ~0V");
    delay(3000);

    // Test MISO (D19), input, just read it
    Serial.println("\n--- Testing MISO (D19) ---");
    pinMode(19, INPUT);
    int misoVal = digitalRead(19);
    Serial.printf("D19 reads: %s\n", misoVal ? "HIGH" : "LOW");
    Serial.println("(HIGH is normal - SD module pulls MISO up)");

    Serial.println("\n========================================");
    Serial.println("Diagnostic complete.");
    Serial.println("If any pin did NOT change on the module,");
    Serial.println("that wire is the problem.");
    Serial.println("========================================");
}

void loop() {
    delay(10000);
}
