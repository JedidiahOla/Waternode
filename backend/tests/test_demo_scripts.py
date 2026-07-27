"""
Tests for seed_db.py and demo_alert.py.

These are the two commands in the README's quick start, and the screenshot in
the README is taken from their output. If tuning CUSUM_H or the warmup window
stops the demo reaching CONFIRMED, that should fail here rather than being
found by someone following the README.
"""

import algorithm
import config
import database as db
import demo_alert
import seed_db


def test_seed_db_populates_a_day_of_readings():
    seed_db.seed()
    count = db.get_reading_count(seed_db.NODE_ID)
    assert count > config.CUSUM_WARMUP_READINGS


def test_seed_db_leaves_the_detector_quiet():
    """Seeded data is clean baseline, so nothing should be alarming."""
    seed_db.seed()
    states = {p: db.get_cusum_state(seed_db.NODE_ID, p)
              for p in algorithm.PARAMETERS}
    assert algorithm.compute_alert_level(states, False) == algorithm.ALERT_NONE


def test_demo_alert_reaches_confirmed():
    seed_db.seed()
    demo_alert.main()

    states = {p: db.get_cusum_state(demo_alert.NODE_ID, p)
              for p in algorithm.PARAMETERS}
    level = algorithm.compute_alert_level(states, False)
    assert level == algorithm.ALERT_CONFIRMED


def test_demo_alert_writes_the_alert_log():
    """The dashboard's alert table is empty without these rows."""
    seed_db.seed()
    demo_alert.main()

    alerts = db.get_recent_alerts(demo_alert.NODE_ID)
    assert alerts, "no alerts logged"
    assert "turbidity" in {a["parameter"] for a in alerts}
    assert all(a["timestamp_fmt"] is not None
               for a in alerts if "timestamp_fmt" in a)


def test_demo_alert_without_seed_data_does_nothing(capsys):
    """Running it on an empty DB should explain itself, not raise."""
    demo_alert.main()

    assert db.get_reading_count(demo_alert.NODE_ID) == 0
    assert "seed_db" in capsys.readouterr().out


def test_demo_event_is_visible_on_the_chart():
    """The injected reading has to land inside the dashboard's 24h window."""
    seed_db.seed()
    demo_alert.main()

    readings = db.get_recent_readings(demo_alert.NODE_ID,
                                      hours=config.DASHBOARD_HOURS)
    peak = max(r["turbidity_ntu"] for r in readings if r["turbidity_ntu"])
    assert peak == demo_alert.EVENT_TURBIDITY_NTU
