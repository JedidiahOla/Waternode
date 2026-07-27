#ifndef RTC_MODULE_H
#define RTC_MODULE_H

#include <Arduino.h>

// RTC MODULE
// DS3231 real-time clock management
// Provides accurate timestamps independent of network connectivity
// NTP sync performed on first boot and periodically to correct drift

// Initialise DS3231 over I2C
// Returns true if RTC found and responding, false if fault
bool rtcInit();

// Get current Unix timestamp from DS3231
// Returns 0 if RTC not available (caller should flag FAULT_RTC_FAIL)
uint32_t rtcGetUnixTime();

// Get current month (1-12) for season index calculation
int rtcGetMonth();

// Get current hour (0-23). Not used by the main wake cycle, transmit
// scheduling is done on elapsed-time deltas, not wall-clock hours, but kept
// for diagnostics and time-of-day-dependent bring-up sketches.
int rtcGetHour();

// Set DS3231 time from Unix timestamp
// Called after NTP sync to initialise or correct the RTC
void rtcSetTime(uint32_t unixTime);

// Check if DS3231 is present and responding
bool rtcIsAvailable();

// Sync DS3231 from NTP server via active GPRS connection
// Must be called AFTER GPRS is connected
// Returns true if sync successful
bool rtcSyncFromNTP();

// Format a Unix timestamp as readable string for logging
// Output written to provided buffer (must be at least 20 chars)
void rtcFormatTimestamp(uint32_t unixTime, char* buffer, size_t bufLen);

#endif // RTC_MODULE_H
