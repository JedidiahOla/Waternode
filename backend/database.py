# DATABASE MODULE
# SQLite schema, initialisation, and all query functions.
# Single-file database, no server, portable, sufficient for this scale.

import logging
import sqlite3
import time
from contextlib import contextmanager

import config

logger = logging.getLogger(__name__)

# Schema

SCHEMA = """
-- Raw readings from sensor node
CREATE TABLE IF NOT EXISTS readings (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp       INTEGER NOT NULL,       -- Unix epoch (from DS3231)
    node_id         TEXT    NOT NULL,
    turbidity_ntu   REAL,
    turbidity_raw   INTEGER,
    tds_mgl         REAL,
    temperature_c   REAL,
    battery_mv      INTEGER,
    alert_level     INTEGER DEFAULT 0,      -- Local alert level from firmware
    fault_flags     INTEGER DEFAULT 0,      -- Bitmask of sensor/system faults
    season_index    INTEGER DEFAULT 0,
    received_at     INTEGER NOT NULL,       -- When backend received this reading
    UNIQUE(node_id, timestamp)
);

-- CUSUM state per parameter, updated on each reading.
-- Storing running state allows the algorithm to resume after backend restart.
CREATE TABLE IF NOT EXISTS cusum_state (
    id                  INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id             TEXT    NOT NULL,
    parameter           TEXT    NOT NULL,   -- 'turbidity', 'tds', 'temperature'
    s_pos               REAL    DEFAULT 0.0,  -- Upper CUSUM accumulator S+
    s_neg               REAL    DEFAULT 0.0,  -- Lower CUSUM accumulator S-
    reading_count       INTEGER DEFAULT 0,    -- Total readings processed
    alarm_active        BOOLEAN DEFAULT 0,    -- Currently in alarm state
    consecutive_alarms  INTEGER DEFAULT 0,    -- For persistence check
    last_alarm_time     INTEGER DEFAULT 0,    -- Unix time of last alarm
    last_sms_time       INTEGER DEFAULT 0,    -- Unix time of last SMS for this param
    UNIQUE(node_id, parameter)
);

-- Alert log, every confirmed alert event
CREATE TABLE IF NOT EXISTS alerts (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp       INTEGER NOT NULL,
    node_id         TEXT    NOT NULL,
    parameter       TEXT    NOT NULL,
    alert_level     INTEGER NOT NULL,
    cusum_score     REAL,
    value_at_alert  REAL,
    baseline_mean   REAL,
    sms_sent        BOOLEAN DEFAULT 0,
    message         TEXT
);

-- System events log (NTP syncs, maintenance, connectivity)
CREATE TABLE IF NOT EXISTS system_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp       INTEGER NOT NULL,
    node_id         TEXT,
    event_type      TEXT    NOT NULL,
    detail          TEXT
);

-- Indices for fast dashboard queries
CREATE INDEX IF NOT EXISTS idx_readings_timestamp  ON readings(timestamp);
CREATE INDEX IF NOT EXISTS idx_readings_node       ON readings(node_id);
CREATE INDEX IF NOT EXISTS idx_alerts_timestamp    ON alerts(timestamp);
"""

# Connection management


@contextmanager
def get_db():
    """Context manager for database connections."""
    conn = sqlite3.connect(config.DATABASE_PATH)
    conn.row_factory = sqlite3.Row   # Rows accessible as dicts
    conn.execute("PRAGMA journal_mode=WAL")   # Better concurrency
    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def init_db():
    """
    Create all tables if they don't exist. Safe to call on every startup:
    CREATE TABLE IF NOT EXISTS / CREATE INDEX IF NOT EXISTS make this
    idempotent, including when multiple app workers each call it once.
    """
    with get_db() as conn:
        conn.executescript(SCHEMA)
    logger.info("Database initialised at %s", config.DATABASE_PATH)


# Readings


def insert_readings(readings_list: list[dict]) -> int:
    """
    Insert a batch of readings from an HTTP POST payload.
    Skips duplicates (same node_id + timestamp) via the UNIQUE constraint.
    Returns the number of new readings inserted.
    """
    inserted = 0
    received_at = int(time.time())

    with get_db() as conn:
        for r in readings_list:
            cursor = conn.execute("""
                INSERT OR IGNORE INTO readings
                (timestamp, node_id, turbidity_ntu, turbidity_raw,
                 tds_mgl, temperature_c, battery_mv,
                 alert_level, fault_flags, season_index, received_at)
                VALUES (?,?,?,?,?,?,?,?,?,?,?)
            """, (
                r.get("ts", 0),
                r.get("node_id", "unknown"),
                r.get("tb"),        # turbidity NTU
                r.get("tbr"),       # turbidity raw ADC
                r.get("td"),        # TDS mg/L
                r.get("tc"),        # temperature C
                r.get("bv"),        # battery mV
                r.get("al", 0),     # alert level
                r.get("ff", 0),     # fault flags
                r.get("si", 0),     # season index
                received_at
            ))
            if cursor.rowcount:
                inserted += 1

    return inserted


def get_recent_readings(node_id: str, hours: int = 24) -> list[dict]:
    """Get readings from the last N hours for dashboard."""
    cutoff = int(time.time()) - (hours * 3600)
    with get_db() as conn:
        rows = conn.execute("""
            SELECT * FROM readings
            WHERE node_id=? AND timestamp>=?
            ORDER BY timestamp ASC
        """, (node_id, cutoff)).fetchall()
    return [dict(r) for r in rows]


def get_latest_reading(node_id: str) -> dict | None:
    """Get the single most recent reading for a node."""
    with get_db() as conn:
        row = conn.execute("""
            SELECT * FROM readings
            WHERE node_id=?
            ORDER BY timestamp DESC LIMIT 1
        """, (node_id,)).fetchone()
    return dict(row) if row else None


def get_reading_count(node_id: str) -> int:
    """Total readings stored for a node."""
    with get_db() as conn:
        row = conn.execute(
            "SELECT COUNT(*) as cnt FROM readings WHERE node_id=?",
            (node_id,)
        ).fetchone()
    return row["cnt"] if row else 0


# CUSUM State


def get_cusum_state(node_id: str, parameter: str) -> dict:
    """
    Retrieve persisted CUSUM state for a parameter.
    Returns a default initial state if no record exists yet.
    """
    with get_db() as conn:
        row = conn.execute("""
            SELECT * FROM cusum_state
            WHERE node_id=? AND parameter=?
        """, (node_id, parameter)).fetchone()

    if row:
        return dict(row)

    return {
        "node_id": node_id,
        "parameter": parameter,
        "s_pos": 0.0,
        "s_neg": 0.0,
        "reading_count": 0,
        "alarm_active": False,
        "consecutive_alarms": 0,
        "last_alarm_time": 0,
        "last_sms_time": 0
    }


def save_cusum_state(node_id: str, parameter: str, state: dict):
    """Upsert CUSUM state for a parameter."""
    with get_db() as conn:
        conn.execute("""
            INSERT INTO cusum_state
            (node_id, parameter, s_pos, s_neg, reading_count,
             alarm_active, consecutive_alarms, last_alarm_time, last_sms_time)
            VALUES (?,?,?,?,?,?,?,?,?)
            ON CONFLICT(node_id, parameter) DO UPDATE SET
                s_pos              = excluded.s_pos,
                s_neg              = excluded.s_neg,
                reading_count      = excluded.reading_count,
                alarm_active       = excluded.alarm_active,
                consecutive_alarms = excluded.consecutive_alarms,
                last_alarm_time    = excluded.last_alarm_time,
                last_sms_time      = excluded.last_sms_time
        """, (
            node_id, parameter,
            state["s_pos"], state["s_neg"],
            state["reading_count"], state["alarm_active"],
            state["consecutive_alarms"],
            state["last_alarm_time"], state["last_sms_time"]
        ))


# Alerts


def insert_alert(node_id: str, timestamp: int, parameter: str,
                  alert_level: int, cusum_score: float,
                  value: float, baseline_mean: float,
                  sms_sent: bool, message: str):
    """Log a confirmed alert event."""
    with get_db() as conn:
        conn.execute("""
            INSERT INTO alerts
            (timestamp, node_id, parameter, alert_level, cusum_score,
             value_at_alert, baseline_mean, sms_sent, message)
            VALUES (?,?,?,?,?,?,?,?,?)
        """, (timestamp, node_id, parameter, alert_level, cusum_score,
              value, baseline_mean, sms_sent, message))


def get_recent_alerts(node_id: str, limit: int = 20) -> list[dict]:
    """Get the most recent alerts for dashboard display."""
    with get_db() as conn:
        rows = conn.execute("""
            SELECT * FROM alerts
            WHERE node_id=?
            ORDER BY timestamp DESC LIMIT ?
        """, (node_id, limit)).fetchall()
    return [dict(r) for r in rows]


# System Events


def log_event(node_id: str, event_type: str, detail: str = ""):
    """Log a system event."""
    with get_db() as conn:
        conn.execute("""
            INSERT INTO system_events (timestamp, node_id, event_type, detail)
            VALUES (?,?,?,?)
        """, (int(time.time()), node_id, event_type, detail))


# Maintenance


def purge_old_readings(days: int | None = None) -> int:
    """
    Delete readings older than the retention period from the DB.
    The SD card archive on the node preserves all data regardless, this
    just keeps the server-side DB lean. Returns the number of rows deleted.
    """
    if days is None:
        days = config.DB_RETENTION_DAYS
    cutoff = int(time.time()) - (days * 86400)
    with get_db() as conn:
        result = conn.execute(
            "DELETE FROM readings WHERE timestamp < ?", (cutoff,)
        )
    logger.info("Purged %d readings older than %d days", result.rowcount, days)
    return result.rowcount
