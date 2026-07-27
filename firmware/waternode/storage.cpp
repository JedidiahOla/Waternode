#include "storage.h"
#include "config.h"
#include <SPI.h>
#include <SD.h>

// Explicit VSPI bus with an explicit begin() call and a speed fallback, // see hardware-bringup/sd_card_test_v2.ino. Some SD module + card
// combinations are unreliable at the default 4MHz on this wiring but work
// fine dropped down to 1MHz, so storageInit() tries both before giving up.
static SPIClass sdSpi(VSPI);
static bool sdAvailable = false;

// Ensures buffer.csv exists and has its header row, created fresh if the
// file doesn't exist yet (first boot on a blank card).
static void ensureBufferFileExists() {
    if (SD.exists(SD_BUFFER_FILE)) return;

    File f = SD.open(SD_BUFFER_FILE, FILE_WRITE);
    if (f) {
        f.print(CSV_HEADER);
        f.close();
    }
}

bool storageInit() {
    sdSpi.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

    if (SD.begin(PIN_SD_CS, sdSpi, 4000000)) {
        sdAvailable = true;
    } else {
        SD.end();
        delay(500);
        if (SD.begin(PIN_SD_CS, sdSpi, 1000000)) {
            Serial.println("[SD] Initialised at fallback 1MHz");
            sdAvailable = true;
        } else {
            Serial.println("[SD] Card not found / not accessible");
            sdAvailable = false;
            return false;
        }
    }

    ensureBufferFileExists();
    return true;
}

bool storageAvailable() {
    return sdAvailable;
}

bool storageWriteReading(uint32_t timestamp,
                          const SensorReading& reading,
                          int alert_level,
                          int season_index) {
    if (!sdAvailable) return false;

    File f = SD.open(SD_BUFFER_FILE, FILE_APPEND);
    if (!f) {
        Serial.println("[SD] Could not open buffer.csv for append");
        return false;
    }

    // Column order must match CSV_HEADER in config.h exactly:
    // timestamp,node_id,turbidity_ntu,turbidity_raw,tds_mgl,temperature_c,
    // battery_mv,alert_level,fault_flags,season_index
    size_t written = f.printf(
        "%u,%s,%.2f,%d,%.2f,%.2f,%d,%d,%d,%d\n",
        (unsigned)timestamp, NODE_ID,
        reading.turbidity_ntu, reading.turbidity_raw,
        reading.tds_mgl, reading.temperature_c,
        reading.battery_mv, alert_level,
        (int)reading.fault_flags, season_index
    );
    f.close();

    if (written == 0) {
        Serial.println("[SD] Write returned 0 bytes - treating as failure");
        return false;
    }
    return true;
}

int storageBufferCount() {
    if (!sdAvailable) return 0;

    File f = SD.open(SD_BUFFER_FILE, FILE_READ);
    if (!f) return 0;

    int count = 0;
    bool skippedHeader = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        if (line.length() == 0) continue;
        if (!skippedHeader) { skippedHeader = true; continue; }
        count++;
    }
    f.close();
    return count;
}

int storageReadBufferAsJSON(char* jsonBuffer, size_t jsonBufSize, int maxRecords) {
    if (!sdAvailable) return -1;

    File f = SD.open(SD_BUFFER_FILE, FILE_READ);
    if (!f) return -1;

    // {"node_id":"WN001","api_key":"...","readings":[{...},{...}]}
    size_t pos = 0;
    auto append = [&](const char* s) -> bool {
        size_t len = strlen(s);
        if (pos + len >= jsonBufSize) return false;   // would overflow
        memcpy(jsonBuffer + pos, s, len);
        pos += len;
        jsonBuffer[pos] = '\0';
        return true;
    };

    char header[160];
    snprintf(header, sizeof(header),
             "{\"node_id\":\"%s\",\"api_key\":\"%s\",\"readings\":[",
             NODE_ID, API_KEY);
    if (!append(header)) { f.close(); return -1; }

    int recordCount = 0;
    bool skippedHeader = false;
    bool firstRecord = true;

    while (f.available() && recordCount < maxRecords) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        if (!skippedHeader) { skippedHeader = true; continue; }

        // Columns: timestamp,node_id,turbidity_ntu,turbidity_raw,tds_mgl,
        //          temperature_c,battery_mv,alert_level,fault_flags,season_index
        char rowCopy[160];
        strncpy(rowCopy, line.c_str(), sizeof(rowCopy) - 1);
        rowCopy[sizeof(rowCopy) - 1] = '\0';

        char* fields[10];
        int fieldCount = 0;
        char* tok = strtok(rowCopy, ",");
        while (tok != nullptr && fieldCount < 10) {
            fields[fieldCount++] = tok;
            tok = strtok(nullptr, ",");
        }
        if (fieldCount != 10) {
            Serial.printf("[SD] Skipping malformed buffer line: %s\n", line.c_str());
            continue;
        }

        // Short keys to save bytes over GPRS, must match the backend's
        // KEY_MAP in algorithm.py exactly.
        char record[192];
        snprintf(record, sizeof(record),
                 "%s{\"ts\":%s,\"tb\":%s,\"tbr\":%s,\"td\":%s,\"tc\":%s,"
                 "\"bv\":%s,\"al\":%s,\"ff\":%s,\"si\":%s}",
                 firstRecord ? "" : ",",
                 fields[0], fields[2], fields[3], fields[4], fields[5],
                 fields[6], fields[7], fields[8], fields[9]);

        if (!append(record)) {
            Serial.println("[SD] JSON buffer full - batch truncated");
            break;
        }
        firstRecord = false;
        recordCount++;
    }
    f.close();

    if (!append("]}")) return -1;
    return recordCount;
}

bool storageArchiveBuffer() {
    if (!sdAvailable) return false;

    File src = SD.open(SD_BUFFER_FILE, FILE_READ);
    if (!src) return false;

    File dst = SD.open(SD_ARCHIVE_FILE, FILE_APPEND);
    if (!dst) {
        src.close();
        return false;
    }

    bool skippedHeader = false;
    while (src.available()) {
        String line = src.readStringUntil('\n');
        if (line.length() == 0) continue;
        if (!skippedHeader) { skippedHeader = true; continue; }   // don't duplicate header into archive
        dst.println(line);
    }
    src.close();
    dst.close();

    // Re-open and confirm it grew. Won't catch every corruption mode, but
    // catches the common one: a partial write leaving the file unchanged.
    File verify = SD.open(SD_ARCHIVE_FILE, FILE_READ);
    bool ok = verify && (verify.size() > 0);
    if (verify) verify.close();

    if (!ok) {
        Serial.println("[SD] Archive write-verify failed - buffer left intact");
        return false;
    }

    // Clear the buffer only after the archive write is verified.
    SD.remove(SD_BUFFER_FILE);
    ensureBufferFileExists();
    return true;
}

void storageLogEvent(uint32_t timestamp, const char* event) {
    if (!sdAvailable) return;

    File f = SD.open(SD_LOG_FILE, FILE_APPEND);
    if (!f) return;
    f.printf("%u,%s\n", (unsigned)timestamp, event);
    f.close();
}

bool storageTrimBuffer(int keepRecords) {
    if (!sdAvailable) return false;

    File src = SD.open(SD_BUFFER_FILE, FILE_READ);
    if (!src) return false;

    // Design target is a 72-hour outage: 288 readings at 15-min intervals.
    // 400 leaves headroom for anomaly mode's 5-min sampling.
    const int MAX_LINES = 400;
    static String lines[MAX_LINES];
    int lineCount = 0;
    bool skippedHeader = false;

    while (src.available() && lineCount < MAX_LINES) {
        String line = src.readStringUntil('\n');
        if (line.length() == 0) continue;
        if (!skippedHeader) { skippedHeader = true; continue; }
        lines[lineCount++] = line;
    }
    src.close();

    int startIdx = (lineCount > keepRecords) ? (lineCount - keepRecords) : 0;

    SD.remove(SD_BUFFER_FILE);
    File dst = SD.open(SD_BUFFER_FILE, FILE_WRITE);
    if (!dst) return false;

    dst.print(CSV_HEADER);
    for (int i = startIdx; i < lineCount; i++) {
        dst.println(lines[i]);
    }
    dst.close();

    Serial.printf("[SD] Trimmed buffer.csv to last %d records\n",
                  lineCount - startIdx);
    return true;
}
