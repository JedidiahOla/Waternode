"""
Unit tests for algorithm.py: CUSUM, Mahalanobis fusion, key normalisation,
alert-level logic. Pure functions only, no Flask or SQLite.
"""

import math

import numpy as np
import pytest

import algorithm
import config


# normalise_reading_keys

def test_normalise_reading_keys_maps_known_short_keys():
    raw = {"ts": 100, "tb": 4.2, "td": 187.3, "tc": 16.2, "node_id": "WN001"}
    result = algorithm.normalise_reading_keys(raw)

    assert result["timestamp"] == 100
    assert result["turbidity_ntu"] == 4.2
    assert result["tds_mgl"] == 187.3
    assert result["temperature_c"] == 16.2
    assert result["node_id"] == "WN001"  # unknown keys pass through unchanged


def test_normalise_reading_keys_does_not_mutate_input():
    raw = {"ts": 100}
    algorithm.normalise_reading_keys(raw)
    assert raw == {"ts": 100}


# Seasonal baseline lookup

@pytest.mark.parametrize("month,expected_season", [
    (1, 0), (4, 0), (11, 0), (12, 0),   # dry
    (5, 1), (6, 1),                      # pre-monsoon
    (7, 2), (8, 2), (9, 2),              # monsoon
    (10, 3),                             # post-monsoon
])
def test_get_season_index(month, expected_season):
    assert algorithm.get_season_index(month) == expected_season


def test_get_baseline_returns_configured_values():
    mean, std = algorithm.get_baseline("turbidity", season_index=0)
    assert mean == config.SEASONAL_BASELINES["turbidity_mean"][0]
    assert std == config.SEASONAL_BASELINES["turbidity_std"][0]


def test_get_baseline_unknown_parameter_falls_back_safely():
    mean, std = algorithm.get_baseline("ph", season_index=0)
    assert (mean, std) == (0.0, 1.0)


def test_get_baseline_never_returns_zero_std():
    # A zero std would blow up normalise() with a division by zero.
    mean, std = algorithm.get_baseline("turbidity", season_index=0)
    assert std > 0


# normalise (z-score)

def test_normalise_zscore():
    assert algorithm.normalise(value=10, mean=10, std=2) == 0
    assert algorithm.normalise(value=14, mean=10, std=2) == 2
    assert algorithm.normalise(value=6, mean=10, std=2) == -2


# CUSUM accumulation

def fresh_state():
    return {
        "s_pos": 0.0, "s_neg": 0.0, "reading_count": 0,
        "alarm_active": False, "consecutive_alarms": 0,
        "last_alarm_time": 0, "last_sms_time": 0,
    }


def test_cusum_ignores_deviation_below_k():
    """z below k shouldn't accumulate. This is the noise filter."""
    state = fresh_state()
    state = algorithm.update_cusum(state, z=config.CUSUM_K - 0.1)
    assert state["s_pos"] == 0.0
    assert state["alarm_active"] is False


def test_cusum_accumulates_sustained_small_deviation():
    """Steady deviation just above k builds S+ over several readings."""
    state = fresh_state()
    z = config.CUSUM_K + 0.3
    for _ in range(5):
        state = algorithm.update_cusum(state, z=z)
    assert state["s_pos"] == pytest.approx(5 * 0.3, abs=1e-9)


def test_cusum_fires_alarm_and_resets_on_threshold_cross():
    state = fresh_state()
    # One huge deviation should jump straight past h.
    state = algorithm.update_cusum(state, z=config.CUSUM_H + 10)
    assert state["alarm_active"] is True
    assert state["consecutive_alarms"] == 1
    # Accumulators reset after firing, ready for the next independent event.
    assert state["s_pos"] == 0.0
    assert state["s_neg"] == 0.0


def test_cusum_downward_shift_uses_s_neg():
    state = fresh_state()
    state = algorithm.update_cusum(state, z=-(config.CUSUM_H + 10))
    assert state["alarm_active"] is True
    assert state["s_neg"] == 0.0  # reset after firing


def test_cusum_consecutive_alarms_wind_down_after_recovery():
    state = fresh_state()
    state = algorithm.update_cusum(state, z=config.CUSUM_H + 10)
    assert state["consecutive_alarms"] == 1

    # A clean reading afterwards should wind the count back down to 0.
    state = algorithm.update_cusum(state, z=0.0)
    assert state["consecutive_alarms"] == 0
    assert state["alarm_active"] is False


def test_cusum_score_reports_peak_before_reset():
    state = fresh_state()
    state = algorithm.update_cusum(state, z=config.CUSUM_H + 2)
    # peak_score should reflect the value that crossed h, not the post-reset 0
    assert algorithm.cusum_score(state) >= config.CUSUM_H


# Mahalanobis fusion

def test_mahalanobis_identity_covariance_is_sum_of_squares():
    z_scores = [1.0, 2.0, 3.0]
    cov = algorithm.build_initial_covariance(len(z_scores))
    d_sq = algorithm.mahalanobis_distance(z_scores, cov)
    assert d_sq == pytest.approx(1 + 4 + 9)


def test_mahalanobis_empty_input_is_zero():
    assert algorithm.mahalanobis_distance([], None) == 0.0


def test_mahalanobis_threshold_matches_degrees_of_freedom():
    assert algorithm.mahalanobis_threshold(3) == pytest.approx(11.345)
    assert algorithm.mahalanobis_threshold(2) == pytest.approx(9.210)
    assert algorithm.mahalanobis_threshold(1) == pytest.approx(6.635)


def test_mahalanobis_threshold_falls_back_for_unexpected_df():
    # Degrade to the most conservative known value rather than raising.
    assert algorithm.mahalanobis_threshold(99) == algorithm.CHI2_CRITICAL_P001[3]


# Warmup

def test_in_warmup_until_enough_readings():
    assert algorithm.in_warmup({"reading_count": 0}) is True
    assert algorithm.in_warmup({"reading_count": config.CUSUM_WARMUP_READINGS - 1}) is True
    assert algorithm.in_warmup({"reading_count": config.CUSUM_WARMUP_READINGS}) is False


def test_in_warmup_defaults_to_true_for_state_without_a_count():
    # Fail safe: unknown history should suppress, not escalate.
    assert algorithm.in_warmup({}) is True


# compute_alert_level

def settled_state(**overrides):
    """Parameter state past the warmup window."""
    state = {
        "alarm_active": False,
        "consecutive_alarms": 0,
        "reading_count": config.CUSUM_WARMUP_READINGS,
    }
    state.update(overrides)
    return state


def test_alert_level_none_when_nothing_active():
    states = {"turbidity": settled_state()}
    assert algorithm.compute_alert_level(states, multivariate_alarm=False) == algorithm.ALERT_NONE


def test_alert_level_confirmed_when_persistence_met():
    states = {"turbidity": settled_state(
        alarm_active=True, consecutive_alarms=config.CUSUM_PERSISTENCE)}
    assert algorithm.compute_alert_level(states, multivariate_alarm=False) == algorithm.ALERT_CONFIRMED


def test_alert_level_watch_when_alarm_active_but_not_yet_persisted():
    states = {"turbidity": settled_state(alarm_active=True, consecutive_alarms=0)}
    # consecutive_alarms below CUSUM_PERSISTENCE -> WATCH, not CONFIRMED
    level = algorithm.compute_alert_level(states, multivariate_alarm=False)
    assert level == algorithm.ALERT_WATCH


def test_alert_level_warning_from_multivariate_alone():
    states = {"turbidity": settled_state()}
    level = algorithm.compute_alert_level(states, multivariate_alarm=True)
    assert level == algorithm.ALERT_WARNING


def test_alert_level_confirmed_overrides_warning():
    states = {"turbidity": settled_state(
        alarm_active=True, consecutive_alarms=config.CUSUM_PERSISTENCE)}
    level = algorithm.compute_alert_level(states, multivariate_alarm=True)
    assert level == algorithm.ALERT_CONFIRMED


def test_alert_level_caps_at_watch_during_warmup():
    """Otherwise the dashboard shows CONFIRMED against an empty alert log."""
    states = {"turbidity": settled_state(
        alarm_active=True,
        consecutive_alarms=config.CUSUM_PERSISTENCE,
        reading_count=0,
    )}
    level = algorithm.compute_alert_level(states, multivariate_alarm=False)
    assert level == algorithm.ALERT_WATCH


def test_alert_level_takes_the_highest_across_parameters():
    states = {
        "turbidity": settled_state(alarm_active=True, consecutive_alarms=0),
        "tds": settled_state(alarm_active=True,
                              consecutive_alarms=config.CUSUM_PERSISTENCE),
        "temperature": settled_state(),
    }
    level = algorithm.compute_alert_level(states, multivariate_alarm=False)
    assert level == algorithm.ALERT_CONFIRMED
