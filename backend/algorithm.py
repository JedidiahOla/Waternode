# CUSUM EARLY WARNING ALGORITHM
# Runs on each incoming reading. Page (1954) cumulative sum control chart.
#
# Per reading: z-score against the seasonal baseline, feed into S+ and S-
# accumulators, alarm if either crosses h, then reset so the next event
# accumulates fresh.
#
# CUSUM_K = deviation ignored as noise per step.
# CUSUM_H = decision threshold. Higher means fewer false alarms, slower
# detection of small shifts.
#
# Also fuses all valid parameters into a Mahalanobis distance.

import logging
import time

import numpy as np

import config
import database as db
import notifications

logger = logging.getLogger(__name__)

# Fault flags, must match the firmware's config.h
FAULT_TURB_SENSOR = 0x01
FAULT_TDS_SENSOR = 0x02
FAULT_TEMP_SENSOR = 0x04
FAULT_TANK_DRY = 0x40

PARAMETERS = ["turbidity", "tds", "temperature"]

# Chi-squared critical values at p=0.01, by degrees of freedom.
# df = number of non-faulted parameters in the reading. A fixed 3-df threshold
# would be wrong whenever a sensor is down.
CHI2_CRITICAL_P001 = {
    1: 6.635,
    2: 9.210,
    3: 11.345,
}

# Alert levels, escalating from no anomaly to a confirmed, SMS-worthy event.
ALERT_NONE = 0
ALERT_WATCH = 1
ALERT_WARNING = 2
ALERT_CONFIRMED = 3

# --- Key mapping ---
# Firmware sends short keys to save bytes over GPRS. Backend uses full names.

KEY_MAP = {
    "ts": "timestamp",
    "tb": "turbidity_ntu",
    "tbr": "turbidity_raw",
    "td": "tds_mgl",
    "tc": "temperature_c",
    "bv": "battery_mv",
    "al": "alert_level",
    "ff": "fault_flags",
    "si": "season_index",
}


def normalise_reading_keys(reading: dict) -> dict:
    """Convert short firmware keys to long keys. Passes through unknown keys."""
    return {KEY_MAP.get(key, key): value for key, value in reading.items()}


# --- Seasonal baseline lookup ---

def get_season_index(month: int) -> int:
    """Month (1-12) -> season index for the baseline arrays."""
    if month in (11, 12, 1, 2, 3, 4):
        return 0    # dry
    elif month in (5, 6):
        return 1    # pre-monsoon
    elif month in (7, 8, 9):
        return 2    # monsoon
    else:
        return 3    # post-monsoon (Oct)


def get_baseline(parameter: str, season_index: int) -> tuple[float, float]:
    """Get (mean, std) for a parameter in a given season."""
    mean_key = f"{parameter}_mean"
    std_key = f"{parameter}_std"

    baselines = config.SEASONAL_BASELINES
    if mean_key not in baselines:
        logger.warning("No baseline configured for parameter %r, "
                        "using (mean=0, std=1)", parameter)
        return (0.0, 1.0)

    mean = baselines[mean_key][season_index]
    std = baselines[std_key][season_index]

    if std <= 0:
        std = 0.1   # avoid divide by zero

    return (mean, std)


def normalise(value: float, mean: float, std: float) -> float:
    """Z-score: (value - mean) / std"""
    return (value - mean) / std


# --- CUSUM ---

def update_cusum(state: dict, z: float) -> dict:
    """Update CUSUM accumulators with a new z-score. Returns updated state."""
    k = config.CUSUM_K
    h = config.CUSUM_H

    state["s_pos"] = max(0.0, state["s_pos"] + z - k)
    state["s_neg"] = max(0.0, state["s_neg"] - z - k)
    state["reading_count"] += 1

    alarm = (state["s_pos"] > h) or (state["s_neg"] > h)

    # Save peak score BEFORE reset, otherwise the logged score would be 0
    state["peak_score"] = max(state["s_pos"], state["s_neg"])

    if alarm:
        state["consecutive_alarms"] += 1
        state["alarm_active"] = True
        state["last_alarm_time"] = int(time.time())
        # Reset so the next event can accumulate fresh
        state["s_pos"] = 0.0
        state["s_neg"] = 0.0
    else:
        # Gradually wind down consecutive count when readings go back to normal
        if state["consecutive_alarms"] > 0:
            state["consecutive_alarms"] = max(0, state["consecutive_alarms"] - 1)
        if state["consecutive_alarms"] == 0:
            state["alarm_active"] = False

    return state


def cusum_score(state: dict) -> float:
    """Peak score (captured before reset so it shows the actual trigger value)."""
    return state.get("peak_score", max(state["s_pos"], state["s_neg"]))


# --- Mahalanobis distance ---
# One number from all valid z-scores. With identity covariance, D^2 = sum(z^2).

def mahalanobis_distance(z_scores: list[float],
                          cov_matrix: np.ndarray | None) -> float:
    """Squared Mahalanobis distance across the given z-scores."""
    if len(z_scores) == 0:
        return 0.0

    z = np.array(z_scores)

    try:
        if cov_matrix is not None and cov_matrix.shape == (len(z), len(z)):
            cov_inv = np.linalg.inv(cov_matrix)
            d_sq = float(z.T @ cov_inv @ z)
        else:
            d_sq = float(np.dot(z, z))  # fall back to sum of squares
    except np.linalg.LinAlgError:
        d_sq = float(np.dot(z, z))      # singular matrix, just use diagonal

    return max(0.0, d_sq)


def mahalanobis_threshold(degrees_of_freedom: int) -> float:
    """
    Chi-squared critical value at p=0.01 for the given df.
    Falls back to the most conservative known value rather than raising.
    """
    if degrees_of_freedom in CHI2_CRITICAL_P001:
        return CHI2_CRITICAL_P001[degrees_of_freedom]
    logger.warning("No chi-squared critical value for df=%d, defaulting to df=3",
                    degrees_of_freedom)
    return CHI2_CRITICAL_P001[3]


def build_initial_covariance(n_params: int) -> np.ndarray:
    """Identity matrix: assumes parameters are independent.
    TODO: estimate real correlations once there's enough field data."""
    return np.eye(n_params)


# --- Alert level ---

def in_warmup(state: dict) -> bool:
    """
    True until a parameter has seen CUSUM_WARMUP_READINGS readings.

    Accumulators run from reading one, but alarms during this window don't
    escalate to CONFIRMED and don't send SMS: the baseline isn't settled yet.
    """
    return state.get("reading_count", 0) < config.CUSUM_WARMUP_READINGS


def compute_alert_level(param_states: dict, multivariate_alarm: bool) -> int:
    """
    Combine per-parameter CUSUM state with the multivariate (Mahalanobis)
    alarm into a single overall alert level:

      NONE (0)      nothing alarming
      WATCH (1)     alarm triggered but not yet persisted, or still in warmup
      WARNING (2)   multivariate (Mahalanobis) alarm active
      CONFIRMED (3) alarm persisted for CUSUM_PERSISTENCE readings, past warmup

    param_states: {parameter: post-update CUSUM state dict}

    Warmup caps a parameter at WATCH, so this agrees with process_reading()'s
    suppression. Otherwise the dashboard would show CONFIRMED against an
    empty alert log.

    At the default CUSUM_PERSISTENCE=1 a parameter jumps NONE -> CONFIRMED on
    its first alarm past warmup, so WATCH is otherwise only reachable via the
    multivariate path. Raising CUSUM_PERSISTENCE changes that, no code edit.
    """
    level = ALERT_NONE

    if multivariate_alarm:
        level = max(level, ALERT_WARNING)

    for state in param_states.values():
        if not state.get("alarm_active"):
            continue
        if in_warmup(state):
            level = max(level, ALERT_WATCH)
        elif state.get("consecutive_alarms", 0) >= config.CUSUM_PERSISTENCE:
            level = ALERT_CONFIRMED
        else:
            level = max(level, ALERT_WATCH)

    return level


# --- Main processing ---

def process_reading(reading: dict) -> dict:
    """Run CUSUM on one reading. Updates DB state, sends alerts if needed."""

    reading = normalise_reading_keys(reading)

    node_id = reading.get("node_id", "unknown")
    timestamp = reading.get("timestamp", int(time.time()))
    season_index = reading.get("season_index", 0)
    fault_flags = reading.get("fault_flags", 0)

    param_values = {
        "turbidity": reading.get("turbidity_ntu"),
        "tds": reading.get("tds_mgl"),
        "temperature": reading.get("temperature_c"),
    }

    # Which sensors are faulted
    param_faults = {
        "turbidity": bool(fault_flags & FAULT_TURB_SENSOR),
        "tds": bool(fault_flags & FAULT_TDS_SENSOR),
        "temperature": bool(fault_flags & FAULT_TEMP_SENSOR),
    }

    # Don't bother processing if the tank is dry
    if fault_flags & FAULT_TANK_DRY:
        return {"status": "tank_dry", "cusum_scores": {}, "alert_level": 0}

    results = {}
    z_scores_valid = []
    confirmed_alarms = []
    param_states_for_level = {}

    # Run CUSUM for each parameter
    for param in PARAMETERS:
        value = param_values.get(param)

        if param_faults[param] or value is None:
            results[param] = {"skipped": True, "reason": "sensor_fault"}
            continue

        mean, std = get_baseline(param, season_index)
        z = normalise(value, mean, std)
        z_scores_valid.append(z)

        state = db.get_cusum_state(node_id, param)
        state = update_cusum(state, z)
        param_states_for_level[param] = state

        # Post-update, so this matches what compute_alert_level() sees below.
        warming_up = in_warmup(state)

        is_confirmed = (
            state["alarm_active"]
            and state["consecutive_alarms"] >= config.CUSUM_PERSISTENCE
            and not warming_up
        )

        if is_confirmed:
            confirmed_alarms.append({
                "param": param,
                "value": value,
                "mean": mean,
                "score": cusum_score(state),
                "state": state,
            })

        results[param] = {
            "z_score": round(z, 3),
            "s_pos": round(state["s_pos"], 3),
            "s_neg": round(state["s_neg"], 3),
            "cusum_score": round(cusum_score(state), 3),
            "alarm": state["alarm_active"],
            "confirmed": is_confirmed,
            "in_warmup": warming_up,
            "baseline_mean": mean,
            "baseline_std": std,
        }

        db.save_cusum_state(node_id, param, state)

    # Needs 2+ valid parameters, and the critical value has to match how many.
    d_squared = 0.0
    multivariate_alarm = False

    if len(z_scores_valid) >= 2:
        cov = build_initial_covariance(len(z_scores_valid))
        d_squared = mahalanobis_distance(z_scores_valid, cov)
        threshold = mahalanobis_threshold(len(z_scores_valid))
        multivariate_alarm = d_squared > threshold

    backend_alert_level = compute_alert_level(param_states_for_level, multivariate_alarm)

    # Send SMS for confirmed alarms
    for alarm in confirmed_alarms:
        param = alarm["param"]
        state = alarm["state"]

        suppression_seconds = config.SMS_SUPPRESSION_MINUTES * 60
        time_since_last_sms = timestamp - state["last_sms_time"]
        sms_allowed = time_since_last_sms > suppression_seconds

        message = (
            f"WATER ALERT CONFIRMED: {param.upper()} anomaly detected. "
            f"Value: {alarm['value']:.1f}, "
            f"Baseline: {alarm['mean']:.1f}. "
            f"CUSUM score: {alarm['score']:.2f}. "
            f"Node {node_id}."
        )

        sms_sent = False
        if sms_allowed:
            sms_sent = notifications.send_sms(message)
            if sms_sent:
                state["last_sms_time"] = timestamp
                db.save_cusum_state(node_id, param, state)

        db.insert_alert(
            node_id=node_id,
            timestamp=timestamp,
            parameter=param,
            alert_level=ALERT_CONFIRMED,
            cusum_score=alarm["score"],
            value=alarm["value"],
            baseline_mean=alarm["mean"],
            sms_sent=sms_sent,
            message=message,
        )

        logger.info("CONFIRMED alarm: %s | value=%.2f | score=%.2f | sms=%s",
                    param, alarm["value"], alarm["score"],
                    "sent" if sms_sent else "suppressed")

    return {
        "status": "processed",
        "backend_alert_level": backend_alert_level,
        "cusum_scores": {p: results[p].get("cusum_score", 0)
                          for p in PARAMETERS if p in results},
        "d_squared": round(d_squared, 3),
        "multivariate_alarm": multivariate_alarm,
        "confirmed_alarms": [a["param"] for a in confirmed_alarms],
        "parameter_detail": results,
    }


def process_batch(readings: list[dict]) -> list[dict]:
    """Process a list of readings in time order."""
    # Readings from the wire use "ts"; ones read back out of the DB use
    # "timestamp". Accept either so the order is right in both cases.
    def when(r):
        return r.get("ts", r.get("timestamp", 0))

    return [process_reading(r) for r in sorted(readings, key=when)]
