"""Tests for database.py, insert dedup, CUSUM state roundtrip, alert log."""

import database as db


def test_insert_readings_deduplicates_by_node_and_timestamp():
    reading = {"ts": 1000, "node_id": "WN001", "tb": 1.0, "td": 100.0, "tc": 15.0}

    first = db.insert_readings([reading])
    second = db.insert_readings([reading])  # same node_id + timestamp again

    assert first == 1
    assert second == 0
    assert db.get_reading_count("WN001") == 1


def test_insert_readings_allows_same_timestamp_different_node():
    reading_a = {"ts": 1000, "node_id": "WN001", "tb": 1.0}
    reading_b = {"ts": 1000, "node_id": "WN002", "tb": 1.0}

    assert db.insert_readings([reading_a]) == 1
    assert db.insert_readings([reading_b]) == 1


def test_get_latest_reading_returns_most_recent():
    db.insert_readings([
        {"ts": 1000, "node_id": "WN001", "tb": 1.0},
        {"ts": 2000, "node_id": "WN001", "tb": 2.0},
    ])
    latest = db.get_latest_reading("WN001")
    assert latest["timestamp"] == 2000
    assert latest["turbidity_ntu"] == 2.0


def test_get_latest_reading_returns_none_when_no_data():
    assert db.get_latest_reading("NO_SUCH_NODE") is None


def test_cusum_state_roundtrip():
    state = db.get_cusum_state("WN001", "turbidity")
    assert state["reading_count"] == 0  # default state

    state["s_pos"] = 3.2
    state["reading_count"] = 5
    state["alarm_active"] = True
    state["consecutive_alarms"] = 2
    db.save_cusum_state("WN001", "turbidity", state)

    reloaded = db.get_cusum_state("WN001", "turbidity")
    assert reloaded["s_pos"] == 3.2
    assert reloaded["reading_count"] == 5
    assert bool(reloaded["alarm_active"]) is True
    assert reloaded["consecutive_alarms"] == 2


def test_insert_and_fetch_alert():
    db.insert_alert(
        node_id="WN001", timestamp=1000, parameter="turbidity",
        alert_level=3, cusum_score=6.2, value=28.4, baseline_mean=3.0,
        sms_sent=True, message="test alert",
    )
    alerts = db.get_recent_alerts("WN001")
    assert len(alerts) == 1
    assert alerts[0]["parameter"] == "turbidity"
    assert alerts[0]["sms_sent"] == 1
