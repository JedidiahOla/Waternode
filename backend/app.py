# WATERNODE BACKEND - FLASK APPLICATION
# API endpoint for sensor node POST + dashboard serving.

import hmac
import logging
import time
from datetime import datetime, timezone
from functools import wraps

from flask import Flask, request, jsonify, render_template, abort

import algorithm
import config
import database as db

logging.basicConfig(
    level=logging.DEBUG if config.DEBUG else logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)

app = Flask(__name__)
app.secret_key = config.SECRET_KEY

# Idempotent (CREATE TABLE IF NOT EXISTS), so safe on every worker start.
db.init_db()


# --- Auth: shared API key ---
# /api/readings reads the key from the JSON body because the SIM800L's
# AT+HTTPDATA can't set custom headers. Dashboard routes use the decorator
# below (x-api-key header or ?api_key=). See README for the caveats.

def valid_api_key(candidate) -> bool:
    """Constant-time key check. compare_digest needs str, so reject anything else."""
    if not isinstance(candidate, str):
        return False
    return hmac.compare_digest(candidate, config.API_KEY)


def require_api_key(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        if not config.DASHBOARD_REQUIRE_AUTH:
            return view(*args, **kwargs)

        key = request.headers.get("x-api-key") or request.args.get("api_key", "")
        if not valid_api_key(key):
            abort(401, description="Invalid or missing API key")
        return view(*args, **kwargs)
    return wrapped


# --- Request validation ---
# The node is the only intended client, but the endpoint is public. Validators
# below return an error string, or None if the input is fine.

MAX_BATCH_SIZE = 500        # ~2x the firmware's BATCH_UPLOAD_MAX

# Plausible range per field. Anything outside these is a corrupt record.
NUMERIC_FIELD_RANGES = {
    "ts":  (0, 4102444800),      # Unix seconds, capped at year 2100
    "tb":  (-1e4, 1e4),          # turbidity NTU
    "tbr": (-1e6, 1e6),          # turbidity raw ADC count
    "td":  (-1e5, 1e5),          # TDS mg/L
    "tc":  (-100, 200),          # temperature C
    "bv":  (0, 20000),           # battery mV
    "al":  (0, 3),               # local alert level
    "ff":  (0, 255),             # fault flag bitmask (one byte)
    "si":  (0, 3),               # season index
}


def _numeric(value) -> bool:
    """True for JSON numbers. bool is an int in Python, so exclude it."""
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def validate_readings(readings) -> str | None:
    """Check a batch of readings. Returns an error message, or None if valid."""
    if len(readings) > MAX_BATCH_SIZE:
        return f"Batch too large: {len(readings)} readings (max {MAX_BATCH_SIZE})"

    for index, reading in enumerate(readings):
        if not isinstance(reading, dict):
            return f"Reading {index} is not an object"

        if reading.get("ts") is None:
            # NOT NULL in the schema, and INSERT OR IGNORE would swallow it.
            return f"Reading {index} is missing required field 'ts'"

        for field, (low, high) in NUMERIC_FIELD_RANGES.items():
            if field not in reading or reading[field] is None:
                continue    # optional: a faulted sensor omits its field
            value = reading[field]
            if not _numeric(value):
                return f"Reading {index}: field {field!r} must be a number"
            if not low <= value <= high:
                return (f"Reading {index}: field {field!r} value {value} "
                        f"is outside the plausible range [{low}, {high}]")

    return None


def int_arg(name: str, default: int, minimum: int, maximum: int) -> int:
    """Integer query param, clamped. Bad input should 400 rather than blow up."""
    raw = request.args.get(name)
    if raw is None:
        return default
    try:
        value = int(raw)
    except (TypeError, ValueError):
        abort(400, description=f"Query parameter {name!r} must be an integer")
    return max(minimum, min(value, maximum))


FAULT_FLAG_LABELS = {
    0x01: "Turbidity sensor fault",
    0x02: "TDS sensor fault",
    0x04: "Temperature sensor fault",
    0x08: "SD card write failure",
    0x10: "GPRS connection failure",
    0x20: "Low battery",
    0x40: "Tank dry / sensor exposed",
    0x80: "RTC fault",
}


def decode_fault_flags(flags: int) -> list[str]:
    """Convert a fault bitmask into a human-readable list."""
    return [label for bit, label in FAULT_FLAG_LABELS.items() if flags & bit]


def format_timestamp(ts: int, fmt: str = "%Y-%m-%d %H:%M UTC") -> str:
    """Unix timestamp -> display string. '-' if missing."""
    if not ts:
        return "-"
    return datetime.fromtimestamp(ts, tz=timezone.utc).strftime(fmt)


def add_alert_timestamps(alerts: list[dict]) -> list[dict]:
    """
    Add timestamp_fmt to each alert row.

    Both /api/latest and /api/alerts feed the same dashboard table, and its
    Time column renders blank without this.
    """
    for alert in alerts:
        alert["timestamp_fmt"] = format_timestamp(alert.get("timestamp", 0))
    return alerts


# --- sensor node API ---

@app.route("/api/readings", methods=["POST"])
def receive_readings():
    """
    Receive a batch of readings from an ESP32 sensor node.

    Expected JSON body:
    {
        "node_id": "WN001",
        "api_key": "...",
        "readings": [
            {
                "ts": 1741478400,
                "tb": 4.2,      turbidity NTU
                "tbr": 2341,    turbidity raw ADC
                "td": 187.3,    TDS mg/L
                "tc": 16.2,     temperature C
                "bv": 3842,     battery mV
                "al": 0,        local alert level
                "ff": 0,        fault flags
                "si": 0         season index
            },
            ...
        ]
    }
    """
    if not request.is_json:
        return jsonify({"error": "Content-Type must be application/json"}), 400

    data = request.get_json(silent=True)
    if not data:
        return jsonify({"error": "Invalid JSON body"}), 400

    # API key travels in the body, see the module docstring above for why.
    if not valid_api_key(data.get("api_key")):
        return jsonify({"error": "Invalid API key"}), 401

    node_id = str(data.get("node_id", "unknown"))
    readings_raw = data.get("readings", [])

    if not isinstance(readings_raw, list) or len(readings_raw) == 0:
        return jsonify({"error": "No readings in payload"}), 400

    # All-or-nothing: the node re-sends its whole buffer next cycle anyway,
    # so rejecting the batch beats reasoning about which half landed.
    error = validate_readings(readings_raw)
    if error:
        logger.warning("Node %s: rejected batch: %s", node_id, error)
        return jsonify({"error": error}), 400

    # Inject node_id into each reading (firmware includes it in the outer object)
    for r in readings_raw:
        r["node_id"] = node_id

    inserted = db.insert_readings(readings_raw)
    cusum_results = algorithm.process_batch(readings_raw)

    db.log_event(node_id, "batch_received",
                 f"Received {len(readings_raw)} readings, "
                 f"inserted {inserted} new")

    logger.info("Node %s: %d received, %d new, CUSUM processed",
                node_id, len(readings_raw), inserted)

    return jsonify({
        "status": "ok",
        "received": len(readings_raw),
        "inserted": inserted,
        "cusum_processed": len(cusum_results),
    }), 200


# --- dashboard API, polled every DASHBOARD_POLL_INTERVAL_SEC by the frontend ---

@app.route("/api/latest", methods=["GET"])
@require_api_key
def api_latest():
    """
    Return current system status for the dashboard: latest readings, CUSUM
    states, recent alerts, and system info.
    """
    node_id = request.args.get("node_id", "WN001")

    latest = db.get_latest_reading(node_id)
    if not latest:
        return jsonify({"status": "no_data", "node_id": node_id}), 200

    cusum_states = {}
    param_states_for_level = {}
    for param in algorithm.PARAMETERS:
        state = db.get_cusum_state(node_id, param)
        season_idx = latest.get("season_index", 0)
        mean, std = algorithm.get_baseline(param, season_idx)
        cusum_states[param] = {
            "s_pos": round(state["s_pos"], 3),
            "s_neg": round(state["s_neg"], 3),
            "alarm_active": state["alarm_active"],
            "consecutive_alarms": state["consecutive_alarms"],
            "reading_count": state["reading_count"],
            "baseline_mean": mean,
            "baseline_std": std,
        }
        param_states_for_level[param] = state

    recent_alerts = add_alert_timestamps(db.get_recent_alerts(node_id, limit=20))

    # Same function the ingestion path uses, so dashboard and SMS logic can't
    # disagree. Multivariate state isn't persisted between requests, so a
    # multivariate-only event shows up a beat late (on the next batch).
    overall_level = algorithm.compute_alert_level(param_states_for_level, False)

    faults = decode_fault_flags(latest.get("fault_flags", 0))

    ts = latest.get("timestamp", 0)
    ts_formatted = format_timestamp(ts, "%Y-%m-%d %H:%M:%S UTC")

    alert_labels = ["NONE", "WATCH", "WARNING", "CONFIRMED"]

    return jsonify({
        "status": "ok",
        "node_id": node_id,
        "timestamp": ts,
        "timestamp_fmt": ts_formatted,
        "overall_alert": overall_level,
        "alert_label": alert_labels[overall_level],
        "readings": {
            "turbidity_ntu": latest.get("turbidity_ntu"),
            "tds_mgl": latest.get("tds_mgl"),
            "temperature_c": latest.get("temperature_c"),
            "battery_mv": latest.get("battery_mv"),
            "season_index": latest.get("season_index", 0),
            "season_name": config.SEASON_NAMES[latest.get("season_index", 0)],
        },
        "cusum": cusum_states,
        "faults": faults,
        "recent_alerts": recent_alerts[:5],   # Last 5 for the status panel
        "total_readings": db.get_reading_count(node_id),
    })


@app.route("/api/history", methods=["GET"])
@require_api_key
def api_history():
    """Return time-series data for the trend charts (last N hours)."""
    node_id = request.args.get("node_id", "WN001")
    hours = int_arg("hours", default=config.DASHBOARD_HOURS,
                    minimum=1, maximum=168)   # 1 hour to 7 days

    readings = db.get_recent_readings(node_id, hours=hours)

    labels = []
    turbidity = []
    tds = []
    temperature = []
    battery = []
    alert_levels = []

    for r in readings:
        ts = r.get("timestamp", 0)
        labels.append(format_timestamp(ts, "%d/%m %H:%M"))
        turbidity.append(r.get("turbidity_ntu"))
        tds.append(r.get("tds_mgl"))
        temperature.append(r.get("temperature_c"))
        battery.append(r.get("battery_mv"))
        alert_levels.append(r.get("alert_level", 0))

    season_idx = readings[-1].get("season_index", 0) if readings else 0
    baselines = {
        "turbidity_mean": config.SEASONAL_BASELINES["turbidity_mean"][season_idx],
        "turbidity_warn": config.SEASONAL_BASELINES["turbidity_mean"][season_idx] * 3,
        "turbidity_who": config.WHO_TURBIDITY_NTU,
        "tds_mean": config.SEASONAL_BASELINES["tds_mean"][season_idx],
        "tds_warn": config.SEASONAL_BASELINES["tds_mean"][season_idx] * 3,
        "tds_who": config.WHO_TDS_MGL,
        "temp_mean": config.SEASONAL_BASELINES["temperature_mean"][season_idx],
    }

    return jsonify({
        "labels": labels,
        "turbidity": turbidity,
        "tds": tds,
        "temperature": temperature,
        "battery": battery,
        "alert_levels": alert_levels,
        "baselines": baselines,
        "count": len(readings),
    })


@app.route("/api/alerts", methods=["GET"])
@require_api_key
def api_alerts():
    """Return full alert history for the dashboard alert log."""
    node_id = request.args.get("node_id", "WN001")
    # SQLite reads a negative LIMIT as "no limit", so clamp before it hits SQL.
    limit = int_arg("limit", default=20, minimum=1, maximum=200)
    alerts = add_alert_timestamps(db.get_recent_alerts(node_id, limit=limit))
    return jsonify({"alerts": alerts, "count": len(alerts)})


# --- dashboard, served as an HTML page ---

@app.route("/")
@app.route("/dashboard")
def dashboard():
    """
    Serve the dashboard page.

    The API key goes into the template so the page's JS can authenticate its
    own /api/* calls. Anyone who loads the page can read the key from source,
    so this only stops the endpoints being crawled or hit directly.
    """
    return render_template(
        "dashboard.html",
        node_id="WN001",
        poll_interval=config.DASHBOARD_POLL_INTERVAL_SEC,
        season_names=config.SEASON_NAMES,
        api_key=config.API_KEY if config.DASHBOARD_REQUIRE_AUTH else "",
    )


@app.route("/health")
def health():
    """Simple health check for uptime monitoring."""
    return jsonify({
        "status": "ok",
        "time": int(time.time()),
        "version": "1.0.0",
    })


@app.errorhandler(401)
def unauthorized(e):
    return jsonify({"error": str(e.description)}), 401


@app.errorhandler(400)
def bad_request(e):
    return jsonify({"error": str(e.description)}), 400


@app.errorhandler(404)
def not_found(e):
    return jsonify({"error": "Not found"}), 404


# ENTRY POINT

if __name__ == "__main__":
    logger.info("=" * 50)
    logger.info("WaterNode Backend v1.0.0")
    logger.info("Database: %s", config.DATABASE_PATH)
    logger.info("SMS: %s", "Enabled (Twilio)" if config.SMS_ENABLED else "Disabled")
    logger.info("Dashboard auth required: %s", config.DASHBOARD_REQUIRE_AUTH)
    logger.info("Debug: %s", config.DEBUG)
    logger.info("=" * 50)
    app.run(host="0.0.0.0", port=config.PORT, debug=config.DEBUG)
