#ifndef ALERT_H
#define ALERT_H

#include <Arduino.h>
#include "sensors.h"

// ALERT MODULE
// Local threshold checking on ESP32, acts as offline-capable SMS failsafe
// Separate from CUSUM which runs on the Flask backend
// Per-parameter alert suppression to prevent SMS spam
// Learning mode handling with WHO absolute fallback

// Alert state for a single parameter, persisted in RTC memory across sleep
struct ParameterAlert {
    int     consecutiveAnomalies;   // Count of consecutive anomalous readings
    int     currentLevel;           // ALERT_NONE through ALERT_CONFIRMED
    uint32_t lastSmsSentTime;       // Unix time of last SMS for this parameter
    bool    alertActive;            // True if currently in alert state
};

// Full alert state, all parameters
// Stored in RTC_DATA_ATTR to survive deep sleep
struct AlertState {
    ParameterAlert turbidity;
    ParameterAlert tds;
    ParameterAlert temperature;
    bool learningMode;              // True during first LEARNING_MODE_DURATION_DAYS
    uint32_t learningStartTime;     // When learning mode began
};

// Initialise alert state (called on first boot only, not after wake from sleep)
void alertInit(AlertState* state, uint32_t currentTime);

// Evaluate a new sensor reading against thresholds
// Updates state in place
// Returns current overall alert level (highest of all parameters)
// Sends SMS if ALERT_CONFIRMED reached and suppression period elapsed
int alertEvaluate(AlertState* state,
                  const SensorReading& reading,
                  int seasonIndex,
                  uint32_t currentTime);

// Check and update learning mode status
// Returns true if still in learning mode after check
bool alertCheckLearningMode(AlertState* state, uint32_t currentTime);

// Get human-readable alert level string
const char* alertLevelString(int level);

// Build alert SMS message string
// Writes into provided buffer (must be >= 160 chars)
void alertBuildSmsMessage(const SensorReading& reading,
                          int alertLevel,
                          const char* parameterName,
                          float currentValue,
                          float baselineMean,
                          uint32_t timestamp,
                          char* buffer,
                          size_t bufLen);

#endif // ALERT_H
