#include "rtc_module.h"
#include "config.h"
#include "connectivity.h"
#include <Wire.h>
#include <RTClib.h>

// DS3231 shares the I2C bus with the ADS1115 (see sensors.cpp). rtcInit() runs
// before sensorsInit(), so this module brings the bus up. Without that,
// RTClib calls Wire.begin() on core defaults, which aren't 21/22 everywhere.
// Wire.begin() is safe to call twice.
static RTC_DS3231 rtc;
static bool rtcAvailable = false;

bool rtcInit() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    if (!rtc.begin()) {
        Serial.println("[RTC] DS3231 not found");
        rtcAvailable = false;
        return false;
    }

    if (rtc.lostPower()) {
        // Backup cell dead or missing: time is wrong until NTP corrects it.
        // Still report available, since I2C works. The caller sets
        // FAULT_RTC_FAIL only when rtcInit() actually fails.
        Serial.println("[RTC] Lost power since last set - time is unreliable "
                        "until NTP sync completes");
    }

    rtcAvailable = true;
    return true;
}

bool rtcIsAvailable() {
    return rtcAvailable;
}

uint32_t rtcGetUnixTime() {
    if (!rtcAvailable) return 0;
    return rtc.now().unixtime();
}

int rtcGetMonth() {
    if (!rtcAvailable) return 1;   // default to a plausible month rather than 0
    return rtc.now().month();
}

int rtcGetHour() {
    if (!rtcAvailable) return 0;
    return rtc.now().hour();
}

void rtcSetTime(uint32_t unixTime) {
    if (!rtcAvailable) return;
    rtc.adjust(DateTime(unixTime));
}

bool rtcSyncFromNTP() {
    // Delegates the actual network round-trip to connectivity.cpp, which
    // owns the SIM800L UART, this module only owns the DS3231 I2C bus.
    // Caller (waternode.ino) is responsible for having GPRS already
    // connected before calling this.
    uint32_t networkTime = 0;
    if (!simGetNetworkTime(&networkTime)) {
        Serial.println("[RTC] NTP sync failed - keeping existing RTC time");
        return false;
    }

    rtcSetTime(networkTime);
    Serial.printf("[RTC] Synced from NTP: unix=%u\n", networkTime);
    return true;
}

void rtcFormatTimestamp(uint32_t unixTime, char* buffer, size_t bufLen) {
    // DateTime's Unix-time constructor is pure calendar math, it doesn't
    // touch the DS3231 hardware, so this works even if the RTC itself isn't
    // available (useful for formatting a timestamp that came from NTP
    // before it's been written back to the RTC).
    DateTime dt(unixTime);
    snprintf(buffer, bufLen, "%04d-%02d-%02d %02d:%02d:%02d",
             dt.year(), dt.month(), dt.day(),
             dt.hour(), dt.minute(), dt.second());
}
