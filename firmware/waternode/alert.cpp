#include "alert.h"
#include "config.h"
#include "connectivity.h"
#include "rtc_module.h"
#include <string.h>
#include <stdio.h>

// Local threshold check: much simpler than the backend's CUSUM, and exists so
// the node can still raise an SMS during a backend or GPRS outage.
// SMS fires on turbidity or TDS only, at 3x the seasonal mean. Temperature is
// tracked (it feeds the alert level driving fast-sample mode) but never sends
// SMS on its own, since there's no WHO limit defined for it.

void alertInit(AlertState* state, uint32_t currentTime) {
    memset(state, 0, sizeof(AlertState));
    state->learningMode = true;
    state->learningStartTime = currentTime;
}

bool alertCheckLearningMode(AlertState* state, uint32_t currentTime) {
    if (state->learningMode) {
        uint32_t elapsedDays = (currentTime - state->learningStartTime) / 86400UL;
        if (elapsedDays >= LEARNING_MODE_DURATION_DAYS) {
            state->learningMode = false;
            Serial.println("[ALERT] Learning mode complete - WHO fallback disabled");
        }
    }
    return state->learningMode;
}

const char* alertLevelString(int level) {
    switch (level) {
        case ALERT_NONE:      return "NONE";
        case ALERT_WATCH:     return "WATCH";
        case ALERT_WARNING:   return "WARNING";
        case ALERT_CONFIRMED: return "CONFIRMED";
        default:              return "UNKNOWN";
    }
}

void alertBuildSmsMessage(const SensorReading& reading,
                           int alertLevel,
                           const char* parameterName,
                           float currentValue,
                           float baselineMean,
                           uint32_t timestamp,
                           char* buffer,
                           size_t bufLen) {
    // Unused in the body, kept in the signature so a future revision can add
    // other channels (e.g. "turbidity also elevated") without an API change.
    (void)reading;
    (void)alertLevel;

    char timeStr[24];
    rtcFormatTimestamp(timestamp, timeStr, sizeof(timeStr));

    snprintf(buffer, bufLen,
             "WATER ALERT (LOCAL), Node %s [%s]: %s reading %.1f exceeds "
             "3x baseline (%.1f). Backend may be unreachable -- inspect "
             "source immediately.",
             NODE_ID, timeStr, parameterName, currentValue, baselineMean);
}

// Per-parameter evaluation

// Returns true if this parameter's reading is over its local alert threshold.
// whoLimit <= 0 means "no WHO fallback defined for this parameter."
static bool exceedsThreshold(float value, float seasonalMean, bool learningMode,
                              float whoLimit) {
    float seasonalThreshold = seasonalMean * LOCAL_THRESHOLD_MULTIPLIER;
    if (value > seasonalThreshold) return true;
    if (learningMode && whoLimit > 0.0f && value > whoLimit) return true;
    return false;
}

// Updates one ParameterAlert in place. sendSmsOnConfirm controls whether this
// parameter is allowed to trigger the local SMS failsafe on its own (only
// turbidity and TDS are, see file header comment).
static void evaluateParameter(ParameterAlert* pa, const char* paramName,
                               float value, float seasonalMean,
                               bool anomalous, uint32_t currentTime,
                               bool sendSmsOnConfirm) {
    if (anomalous) {
        pa->consecutiveAnomalies++;
    } else {
        // Decay rather than reset; one clean reading shouldn't clear a run.
        if (pa->consecutiveAnomalies > 0) pa->consecutiveAnomalies--;
    }

    if (pa->consecutiveAnomalies <= 0) {
        pa->consecutiveAnomalies = 0;
        pa->currentLevel = ALERT_NONE;
        pa->alertActive = false;
        return;
    }

    pa->alertActive = true;
    if (pa->consecutiveAnomalies >= ALERT_PERSISTENCE) {
        pa->currentLevel = ALERT_CONFIRMED;
    } else if (pa->consecutiveAnomalies >= 2) {
        pa->currentLevel = ALERT_WARNING;
    } else {
        pa->currentLevel = ALERT_WATCH;
    }

    if (sendSmsOnConfirm && pa->currentLevel == ALERT_CONFIRMED) {
        uint32_t secondsSinceLastSms = currentTime - pa->lastSmsSentTime;
        bool suppressed = pa->lastSmsSentTime != 0 &&
                           secondsSinceLastSms < (uint32_t)(SMS_SUPPRESSION_MINUTES * 60);

        if (!suppressed) {
            char message[160];
            alertBuildSmsMessage({}, pa->currentLevel, paramName, value,
                                 seasonalMean, currentTime, message, sizeof(message));
            if (smsSendAlert(message)) {
                pa->lastSmsSentTime = currentTime;
                Serial.printf("[ALERT] Local SMS sent for %s\n", paramName);
            } else {
                Serial.printf("[ALERT] Local SMS FAILED for %s\n", paramName);
            }
        }
    }
}

int alertEvaluate(AlertState* state,
                   const SensorReading& reading,
                   int seasonIndex,
                   uint32_t currentTime) {
    alertCheckLearningMode(state, currentTime);

    if (reading.tank_dry) {
        return ALERT_NONE;   // mirrors the backend's FAULT_TANK_DRY handling
    }

    int overall = ALERT_NONE;

    if (!(reading.fault_flags & FAULT_TURB_SENSOR)) {
        float mean = TURB_MEAN[seasonIndex];
        bool anomalous = exceedsThreshold(reading.turbidity_ntu, mean,
                                           state->learningMode, WHO_TURBIDITY_LIMIT_NTU);
        evaluateParameter(&state->turbidity, "TURBIDITY", reading.turbidity_ntu,
                          mean, anomalous, currentTime, /*sendSmsOnConfirm=*/true);
        if (state->turbidity.currentLevel > overall) overall = state->turbidity.currentLevel;
    }

    if (!(reading.fault_flags & FAULT_TDS_SENSOR)) {
        float mean = TDS_MEAN[seasonIndex];
        bool anomalous = exceedsThreshold(reading.tds_mgl, mean,
                                           state->learningMode, WHO_TDS_LIMIT_MGL);
        evaluateParameter(&state->tds, "TDS", reading.tds_mgl,
                          mean, anomalous, currentTime, /*sendSmsOnConfirm=*/true);
        if (state->tds.currentLevel > overall) overall = state->tds.currentLevel;
    }

    if (!(reading.fault_flags & FAULT_TEMP_SENSOR)) {
        float mean = TEMP_MEAN[seasonIndex];
        // No WHO temperature limit is defined for this system (see file
        // header), seasonal multiplier only, and never sends its own SMS.
        bool anomalous = exceedsThreshold(reading.temperature_c, mean,
                                           state->learningMode, /*whoLimit=*/0.0f);
        evaluateParameter(&state->temperature, "TEMPERATURE", reading.temperature_c,
                          mean, anomalous, currentTime, /*sendSmsOnConfirm=*/false);
        if (state->temperature.currentLevel > overall) overall = state->temperature.currentLevel;
    }

    return overall;
}
