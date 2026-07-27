#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include "sensors.h"

// STORAGE MODULE
// SD card CSV operations: buffer.csv and archive.csv
// buffer.csv, unsynced readings awaiting upload
// archive.csv, successfully uploaded readings (permanent record)
// system.log, system events, faults, maintenance notes

// Initialise SD card, returns true if SD found and accessible
bool storageInit();

// Check if SD card is available
bool storageAvailable();

// Write a single reading to buffer.csv
// timestamp: Unix epoch from DS3231
// reading: sensor data struct
// alert_level: current alert state (ALERT_NONE through ALERT_CONFIRMED)
// season_index: current season (0-3)
// Returns true if write succeeded
bool storageWriteReading(uint32_t timestamp,
                         const SensorReading& reading,
                         int alert_level,
                         int season_index);

// Count records currently in buffer.csv
int storageBufferCount();

// Read buffer.csv contents into a JSON array string for HTTP POST
// Output written to provided buffer, must be large enough
// maxRecords limits how many records to include per batch
// Returns number of records included, -1 on error
int storageReadBufferAsJSON(char* jsonBuffer, size_t jsonBufSize, int maxRecords);

// After successful upload, move buffer records to archive
// Appends buffer.csv contents to archive.csv, then clears buffer.csv
// Returns true if successful
bool storageArchiveBuffer();

// Write a timestamped event to system.log
// Used for maintenance events, NTP syncs, connectivity events, etc.
void storageLogEvent(uint32_t timestamp, const char* event);

// Delete buffer.csv contents (used after failed upload, preserves last N records)
// Keeps last 'keepRecords' records in buffer for next attempt
bool storageTrimBuffer(int keepRecords);

#endif // STORAGE_H
