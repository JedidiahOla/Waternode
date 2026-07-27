#include "connectivity.h"
#include "config.h"
#include <HardwareSerial.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// UART2 to the SIM800L. ESP32 has 3 hardware UARTs; UART0 is used by
// Serial (USB debug), so this uses UART2.
static HardwareSerial sim800l(2);

// Forward declaration, defined near the bottom of this file, used throughout.
static bool waitForResponseCaptureAT(const char* cmd, char* outBuffer, size_t outBufSize);

// Low-level AT command helpers

// Reads until `expected` appears, or timeoutMs elapses. Discards what it reads.
// The delay(1) matters: some waits run 15s, and spinning the core that long
// starves other tasks and burns battery. delay() yields and lets the CPU idle.
static bool waitForResponse(const char* expected, uint32_t timeoutMs) {
    String buffer;
    uint32_t start = millis();

    while (millis() - start < timeoutMs) {
        if (!sim800l.available()) {
            delay(1);
            continue;
        }
        while (sim800l.available()) {
            buffer += (char)sim800l.read();
            if (buffer.indexOf(expected) != -1) {
                return true;
            }
        }
    }
    return false;
}

// Same as waitForResponse, but also copies everything received into
// outBuffer (useful for parsing AT+CSQ, AT+CCLK? etc, not just checking OK).
static bool waitForResponseCapture(const char* expected, uint32_t timeoutMs,
                                    char* outBuffer, size_t outBufSize) {
    size_t written = 0;
    uint32_t start = millis();
    String buffer;

    while (millis() - start < timeoutMs) {
        if (!sim800l.available()) {
            delay(1);      // yield, see waitForResponse()
            continue;
        }
        while (sim800l.available()) {
            char c = (char)sim800l.read();
            buffer += c;
            if (written + 1 < outBufSize) {
                outBuffer[written++] = c;
            }
            if (buffer.indexOf(expected) != -1) {
                if (outBufSize > 0) outBuffer[written] = '\0';
                return true;
            }
        }
    }
    if (outBufSize > 0) outBuffer[written] = '\0';
    return false;
}

// Sends a bare AT command (CRLF appended) and waits for the expected token.
static bool sendAT(const char* cmd, const char* expected = "OK",
                    uint32_t timeoutMs = AT_COMMAND_TIMEOUT_MS) {
    // Flush anything stale sitting in the RX buffer before we send, so a
    // leftover URC (unsolicited result code) from a previous command
    // doesn't get mistaken for this command's response.
    while (sim800l.available()) sim800l.read();

    sim800l.print(cmd);
    sim800l.print("\r\n");
    return waitForResponse(expected, timeoutMs);
}

// Power-on / recovery
// Power-cycles the module via PWRKEY if it isn't answering plain "AT",
// which is the usual recovery after a failed GPRS attempt leaves the
// module in an unresponsive state.

static bool sim800lReady() {
    for (int attempt = 0; attempt < SIM_BOOT_RETRY_ATTEMPTS; attempt++) {
        if (sendAT("AT", "OK", 1000)) {
            return true;
        }

        Serial.printf("[SIM] Not responding (attempt %d/%d) - power-cycling\n",
                      attempt + 1, SIM_BOOT_RETRY_ATTEMPTS);

        // Pull PWRKEY low for 1.5s then release, per the SIM800L hardware
        // design guide power-on/reset sequence.
        digitalWrite(PIN_SIM_PWRKEY, LOW);
        delay(1500);
        digitalWrite(PIN_SIM_PWRKEY, HIGH);
        delay(3000);   // wait for the module to boot
    }
    return false;   // unresponsive after all attempts
}

void simInit() {
    pinMode(PIN_SIM_PWRKEY, OUTPUT);
    digitalWrite(PIN_SIM_PWRKEY, HIGH);   // idle high between power-cycles

    sim800l.begin(GSM_BAUD_RATE, SERIAL_8N1, PIN_SIM_RX, PIN_SIM_TX);
    delay(100);

    // Bring the module out of low-power mode in case it was left there by
    // simSleep() on a previous cycle, harmless no-op if it's already awake.
    sendAT("AT+CFUN=1", "OK", 5000);
}

bool simWaitBoot() {
    if (!sim800lReady()) {
        Serial.println("[SIM] Module unresponsive after all recovery attempts");
        return false;
    }

    sendAT("ATE0");             // echo off, simplifies response parsing
    sendAT("AT+CMEE=0");        // plain "ERROR", not verbose error codes
    return true;
}

// Network status

bool simNetworkRegistered() {
    char resp[64];
    if (!waitForResponseCaptureAT("AT+CREG?", resp, sizeof(resp))) {
        return false;
    }
    // Response looks like: +CREG: 0,1   (registered, home network)
    //                  or:  +CREG: 0,5  (registered, roaming)
    return (strstr(resp, ",1") != nullptr) || (strstr(resp, ",5") != nullptr);
}

int simSignalQuality() {
    char resp[64];
    if (!waitForResponseCaptureAT("AT+CSQ", resp, sizeof(resp))) {
        return 99;   // unknown
    }
    // Response looks like: +CSQ: 18,0
    char* p = strstr(resp, "+CSQ:");
    if (!p) return 99;
    return atoi(p + 5);
}

// GPRS bearer

bool gprsConnect() {
    sendAT("AT+SAPBR=3,1,\"Contype\",\"GPRS\"");
    char apnCmd[96];
    snprintf(apnCmd, sizeof(apnCmd), "AT+SAPBR=3,1,\"APN\",\"%s\"", APN);
    sendAT(apnCmd);

    if (strlen(APN_USER) > 0) {
        char userCmd[64];
        snprintf(userCmd, sizeof(userCmd), "AT+SAPBR=3,1,\"USER\",\"%s\"", APN_USER);
        sendAT(userCmd);
    }
    if (strlen(APN_PASS) > 0) {
        char passCmd[64];
        snprintf(passCmd, sizeof(passCmd), "AT+SAPBR=3,1,\"PWD\",\"%s\"", APN_PASS);
        sendAT(passCmd);
    }

    // Open the bearer, this is the step that actually attaches to GPRS and
    // can legitimately take several seconds on a weak signal.
    if (!sendAT("AT+SAPBR=1,1", "OK", GPRS_BEARER_TIMEOUT_MS)) {
        Serial.println("[GPRS] Bearer failed to open");
        return false;
    }

    // Confirm an IP was actually assigned.
    char resp[64];
    if (!waitForResponseCaptureAT("AT+SAPBR=2,1", resp, sizeof(resp))) {
        return false;
    }
    // Response: +SAPBR: 1,1,"10.x.x.x", the third field is "0.0.0.0" if
    // no IP was actually assigned even though the bearer command returned OK.
    bool hasIp = (strstr(resp, "\"0.0.0.0\"") == nullptr) &&
                 (strstr(resp, "+SAPBR:") != nullptr);
    if (!hasIp) {
        Serial.println("[GPRS] Bearer opened but no IP assigned");
    }
    return hasIp;
}

void gprsDisconnect() {
    sendAT("AT+SAPBR=0,1", "OK", 5000);
}

// HTTP POST

static bool httpPostOnce(const char* jsonPayload) {
    size_t payloadLen = strlen(jsonPayload);

    sendAT("AT+HTTPTERM", "OK", 2000);   // clean up any stale session first
    if (!sendAT("AT+HTTPINIT")) return false;

    sendAT("AT+HTTPPARA=\"CID\",1");

    char urlCmd[160];
    snprintf(urlCmd, sizeof(urlCmd), "AT+HTTPPARA=\"URL\",\"%s%s\"",
             SERVER_URL, API_ENDPOINT);
    sendAT(urlCmd);

    sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"");

    char dataCmd[48];
    snprintf(dataCmd, sizeof(dataCmd), "AT+HTTPDATA=%u,%u",
             (unsigned)payloadLen, (unsigned)HTTP_TIMEOUT_MS);
    // Module replies "DOWNLOAD" to signal it's ready for the raw bytes.
    if (!sendAT(dataCmd, "DOWNLOAD", 5000)) {
        sendAT("AT+HTTPTERM", "OK", 2000);
        return false;
    }
    sim800l.print(jsonPayload);
    if (!waitForResponse("OK", HTTP_TIMEOUT_MS)) {
        sendAT("AT+HTTPTERM", "OK", 2000);
        return false;
    }

    // AT+HTTPACTION=1 issues the POST. The actual result arrives later as an
    // unsolicited "+HTTPACTION: 1,<status>,<len>" line, not as this command's
    // immediate response, the immediate response is just "OK" to say the
    // action started.
    if (!sendAT("AT+HTTPACTION=1", "OK", 5000)) {
        sendAT("AT+HTTPTERM", "OK", 2000);
        return false;
    }

    char resp[64];
    bool gotAction = waitForResponseCapture("+HTTPACTION:", HTTP_TIMEOUT_MS,
                                             resp, sizeof(resp));
    sendAT("AT+HTTPTERM", "OK", 2000);

    if (!gotAction) {
        Serial.println("[HTTP] No +HTTPACTION response before timeout");
        return false;
    }

    // Response: +HTTPACTION: 1,200,123  (method, status, response length)
    char* p = strstr(resp, "+HTTPACTION:");
    if (!p) return false;
    int method = 0, status = 0, len = 0;
    sscanf(p, "+HTTPACTION: %d,%d,%d", &method, &status, &len);

    if (status != 200) {
        Serial.printf("[HTTP] Non-200 response: %d\n", status);
        return false;
    }
    return true;
}

bool httpPost(const char* jsonPayload) {
    const int maxAttempts = 3;
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        if (httpPostOnce(jsonPayload)) {
            return true;
        }
        if (attempt < maxAttempts - 1) {
            uint32_t backoffMs = 1000UL << attempt;   // 1s, 2s, 4s
            Serial.printf("[HTTP] Attempt %d failed, retrying in %ums\n",
                          attempt + 1, backoffMs);
            delay(backoffMs);
        }
    }
    return false;
}

// SMS

static bool smsSendTo(const char* number, const char* message) {
    if (!sendAT("AT+CMGF=1")) return false;   // text mode

    char cmgsCmd[48];
    snprintf(cmgsCmd, sizeof(cmgsCmd), "AT+CMGS=\"%s\"", number);

    // The module replies with "> " prompting for the message body.
    if (!sendAT(cmgsCmd, ">", 5000)) return false;

    sim800l.print(message);
    sim800l.write(0x1A);   // Ctrl+Z sends the message

    return waitForResponse("+CMGS:", 10000);
}

bool smsSendAlert(const char* message) {
    bool sentAny = false;

    if (strlen(SMS_NUMBER_1) > 0) sentAny |= smsSendTo(SMS_NUMBER_1, message);
    if (strlen(SMS_NUMBER_2) > 0) sentAny |= smsSendTo(SMS_NUMBER_2, message);
    if (strlen(SMS_NUMBER_3) > 0) sentAny |= smsSendTo(SMS_NUMBER_3, message);

    return sentAny;
}

// NTP time (via GPRS, must be called after gprsConnect() succeeds)

bool simGetNetworkTime(uint32_t* unixTimeOut) {
    // Sync to UTC (offset 0), not local time. mktime() below has no TZ set so
    // it treats its input as UTC; asking for local time here would put every
    // timestamp out by 5h45m. NTP_GMT_OFFSET_SEC is for display only.
    char ntpCmd[64];
    snprintf(ntpCmd, sizeof(ntpCmd), "AT+CNTP=\"%s\",0", NTP_SERVER);
    sendAT(ntpCmd, "OK", 5000);

    // AT+CNTP triggers an async "+CNTP: 1" success URC once the sync completes.
    if (!waitForResponse("+CNTP: 1", NTP_SYNC_TIMEOUT_MS)) {
        Serial.println("[NTP] Sync did not complete in time");
        return false;
    }

    char resp[48];
    if (!waitForResponseCaptureAT("AT+CCLK?", resp, sizeof(resp))) {
        return false;
    }

    // Response: +CCLK: "26/07/23,14:32:10+00"
    char* p = strstr(resp, "+CCLK:");
    if (!p) return false;

    int yy, mm, dd, hh, min_, ss;
    if (sscanf(p, "+CCLK: \"%d/%d/%d,%d:%d:%d", &yy, &mm, &dd, &hh, &min_, &ss) != 6) {
        Serial.println("[NTP] Could not parse +CCLK response");
        return false;
    }

    // Convert to Unix time (UTC, the module's clock is set to +00 by CNTP).
    struct tm t = {};
    t.tm_year = (yy < 70 ? 2000 + yy : 1900 + yy) - 1900;
    t.tm_mon  = mm - 1;
    t.tm_mday = dd;
    t.tm_hour = hh;
    t.tm_min  = min_;
    t.tm_sec  = ss;
    *unixTimeOut = (uint32_t)mktime(&t);
    return true;
}

// Sleep

void simSleep() {
    sendAT("AT+CFUN=0", "OK", 5000);
}

// Small helper used by the AT+CREG? / AT+CSQ / AT+SAPBR=2,1 / AT+CCLK? call
// sites above, sends a command and captures the raw response for parsing,
// rather than just checking for a fixed "OK" token.
static bool waitForResponseCaptureAT(const char* cmd, char* outBuffer, size_t outBufSize) {
    while (sim800l.available()) sim800l.read();
    sim800l.print(cmd);
    sim800l.print("\r\n");
    return waitForResponseCapture("OK", AT_COMMAND_TIMEOUT_MS, outBuffer, outBufSize);
}
