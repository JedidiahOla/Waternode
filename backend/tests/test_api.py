"""
Tests for the Flask layer.

The node POSTs to a public URL, so every route here is reachable by anything
that finds the address. Malformed input must come back as a 4xx: a 500 means
an unhandled exception escaped the view.
"""

import json
import pathlib

import pytest

import app as flask_app
import config


VALID_READING = {
    "ts": 1741478400,
    "tb": 4.2,      # turbidity NTU
    "tbr": 2341,    # turbidity raw ADC
    "td": 187.3,    # TDS mg/L
    "tc": 16.2,     # temperature C
    "bv": 3842,     # battery mV
    "al": 0,        # local alert level
    "ff": 0,        # fault flags
    "si": 0,        # season index
}


@pytest.fixture
def client():
    flask_app.app.config["TESTING"] = True
    with flask_app.app.test_client() as c:
        yield c


@pytest.fixture
def auth_required(monkeypatch):
    """Turn dashboard auth on. conftest disables it by default."""
    monkeypatch.setattr(config, "DASHBOARD_REQUIRE_AUTH", True)


def post_readings(client, readings, api_key=None, node_id="WN001"):
    payload = {
        "node_id": node_id,
        "api_key": config.API_KEY if api_key is None else api_key,
        "readings": readings,
    }
    return client.post("/api/readings", data=json.dumps(payload),
                       content_type="application/json")


# --- configuration ---

def test_dotenv_is_loaded_by_config():
    """
    The README's setup is: cp .env.example .env, fill it in, run. That only
    works if something actually reads .env. Regression: nothing did, so the
    documented first command crashed.
    """
    import config
    assert "load_dotenv" in pathlib.Path(config.__file__).read_text()


def test_env_example_lists_every_required_setting():
    """A setting that isn't in .env.example is one nobody knows to set."""
    example = pathlib.Path(__file__).resolve().parent.parent / ".env.example"
    documented = {line.split("=")[0].strip()
                  for line in example.read_text().splitlines()
                  if "=" in line and not line.strip().startswith("#")}
    for required in ("SECRET_KEY", "API_KEY", "DATABASE_PATH", "DEBUG"):
        assert required in documented, f"{required} missing from .env.example"


# --- /health ---

def test_health_needs_no_auth(client):
    response = client.get("/health")
    assert response.status_code == 200
    assert response.get_json()["status"] == "ok"


# --- /api/readings, auth ---

def test_ingestion_accepts_a_valid_batch(client):
    response = post_readings(client, [VALID_READING])
    assert response.status_code == 200

    body = response.get_json()
    assert body["received"] == 1
    assert body["inserted"] == 1


def test_ingestion_rejects_a_wrong_api_key(client):
    response = post_readings(client, [VALID_READING], api_key="not-the-key")
    assert response.status_code == 401


def test_ingestion_rejects_a_missing_api_key(client):
    response = client.post("/api/readings",
                           data=json.dumps({"node_id": "WN001",
                                            "readings": [VALID_READING]}),
                           content_type="application/json")
    assert response.status_code == 401


def test_ingestion_rejects_a_non_string_api_key(client):
    # compare_digest raises TypeError on non-str, which would be a 500.
    response = post_readings(client, [VALID_READING], api_key={"nested": "object"})
    assert response.status_code == 401


# --- /api/readings, payload validation ---

def test_ingestion_rejects_a_non_json_body(client):
    response = client.post("/api/readings", data="not json",
                           content_type="text/plain")
    assert response.status_code == 400


def test_ingestion_rejects_an_empty_batch(client):
    assert post_readings(client, []).status_code == 400


def test_ingestion_rejects_readings_that_are_not_objects(client):
    # Regression: used to TypeError on r["node_id"] = node_id -> 500.
    response = post_readings(client, ["junk"])
    assert response.status_code == 400


def test_ingestion_rejects_a_non_numeric_sensor_value(client):
    # Regression: a string used to reach the z-score maths and blow up.
    bad = dict(VALID_READING, tb="hello")
    response = post_readings(client, [bad])
    assert response.status_code == 400


def test_ingestion_rejects_a_reading_with_no_timestamp(client):
    bad = {key: value for key, value in VALID_READING.items() if key != "ts"}
    response = post_readings(client, [bad])
    assert response.status_code == 400


def test_ingestion_rejects_a_null_timestamp(client):
    # NOT NULL in the schema, but INSERT OR IGNORE swallows it: the row would
    # silently vanish behind a 200.
    response = post_readings(client, [dict(VALID_READING, ts=None)])
    assert response.status_code == 400


def test_ingestion_rejects_an_implausible_value(client):
    response = post_readings(client, [dict(VALID_READING, tc=5000)])
    assert response.status_code == 400


def test_ingestion_rejects_an_oversized_batch(client):
    batch = [dict(VALID_READING, ts=VALID_READING["ts"] + i)
             for i in range(flask_app.MAX_BATCH_SIZE + 1)]
    assert post_readings(client, batch).status_code == 400


def test_ingestion_rejects_a_batch_atomically(client):
    """One bad reading rejects the whole batch. Nothing partially stored."""
    batch = [VALID_READING, dict(VALID_READING, ts=1741478500, td="bad")]
    assert post_readings(client, batch).status_code == 400

    import database as db
    assert db.get_reading_count("WN001") == 0


def test_ingestion_allows_omitted_optional_fields(client):
    """A faulted sensor omits its field. Not an error."""
    partial = {"ts": 1741478400, "tc": 16.2, "ff": 1}
    assert post_readings(client, [partial]).status_code == 200


def test_ingestion_deduplicates_a_replayed_batch(client):
    """The node re-sends its buffer if it missed our 200, so replay must be safe."""
    assert post_readings(client, [VALID_READING]).get_json()["inserted"] == 1
    assert post_readings(client, [VALID_READING]).get_json()["inserted"] == 0


# --- dashboard endpoints, auth ---

@pytest.mark.parametrize("route", ["/api/latest", "/api/history", "/api/alerts"])
def test_dashboard_endpoints_reject_a_missing_key(client, auth_required, route):
    assert client.get(route).status_code == 401


@pytest.mark.parametrize("route", ["/api/latest", "/api/history", "/api/alerts"])
def test_dashboard_endpoints_accept_a_header_key(client, auth_required, route):
    response = client.get(route, headers={"x-api-key": config.API_KEY})
    assert response.status_code == 200


def test_dashboard_endpoints_accept_a_query_param_key(client, auth_required):
    response = client.get(f"/api/latest?api_key={config.API_KEY}")
    assert response.status_code == 200


def test_dashboard_endpoints_are_open_when_auth_is_disabled(client):
    # DASHBOARD_REQUIRE_AUTH=false is the local-demo case.
    assert client.get("/api/latest").status_code == 200


# --- dashboard endpoints, query parameter handling ---

@pytest.mark.parametrize("query", ["hours=abc", "hours=", "hours=1.5"])
def test_history_rejects_a_non_integer_hours(client, query):
    # Regression: int() on these used to raise ValueError -> 500.
    assert client.get(f"/api/history?{query}").status_code == 400


def test_alerts_rejects_a_non_integer_limit(client):
    assert client.get("/api/alerts?limit=xyz").status_code == 400


def test_history_clamps_an_out_of_range_hours(client):
    for query in ("hours=-5", "hours=100000"):
        assert client.get(f"/api/history?{query}").status_code == 200


def test_alerts_clamps_a_negative_limit(client):
    """SQLite reads a negative LIMIT as 'no limit'. Must not reach the query."""
    post_readings(client, [VALID_READING])
    response = client.get("/api/alerts?limit=-1")
    assert response.status_code == 200
    assert len(response.get_json()["alerts"]) <= 200


# --- dashboard endpoints, response shape ---

def test_latest_reports_no_data_for_an_unknown_node(client):
    response = client.get("/api/latest?node_id=NO_SUCH_NODE")
    assert response.status_code == 200
    assert response.get_json()["status"] == "no_data"


def test_latest_returns_the_most_recent_reading(client):
    post_readings(client, [VALID_READING])
    body = client.get("/api/latest").get_json()

    assert body["status"] == "ok"
    assert body["readings"]["turbidity_ntu"] == VALID_READING["tb"]
    assert body["alert_label"] in ("NONE", "WATCH", "WARNING", "CONFIRMED")
    assert set(body["cusum"]) == set(flask_app.algorithm.PARAMETERS)


def test_latest_formats_alert_timestamps(client):
    """Regression: /api/latest omitted timestamp_fmt, blanking the Time column."""
    import database as db
    db.insert_alert(node_id="WN001", timestamp=1741478400, parameter="turbidity",
                    alert_level=3, cusum_score=9.1, value=28.4, baseline_mean=1.0,
                    sms_sent=False, message="test alert")
    post_readings(client, [VALID_READING])

    alert = client.get("/api/latest").get_json()["recent_alerts"][0]
    assert alert["timestamp_fmt"].startswith("2025-")


def test_history_returns_parallel_series(client):
    post_readings(client, [VALID_READING,
                           dict(VALID_READING, ts=VALID_READING["ts"] + 900)])
    body = client.get("/api/history").get_json()

    # The chart code zips these by index, so lengths must match.
    assert len(body["labels"]) == body["count"]
    for series in ("turbidity", "tds", "temperature", "battery"):
        assert len(body[series]) == body["count"]


def test_history_of_an_empty_database_is_not_an_error(client):
    response = client.get("/api/history")
    assert response.status_code == 200
    assert response.get_json()["count"] == 0


# --- fault flag decoding ---

def test_fault_flags_decode_to_labels(client):
    post_readings(client, [dict(VALID_READING, ff=0x01 | 0x20)])
    faults = client.get("/api/latest").get_json()["faults"]

    assert "Turbidity sensor fault" in faults
    assert "Low battery" in faults
    assert len(faults) == 2


def test_no_fault_flags_decodes_to_an_empty_list(client):
    assert flask_app.decode_fault_flags(0) == []


# --- dashboard page ---

def test_dashboard_page_renders(client):
    response = client.get("/")
    assert response.status_code == 200
    assert b"WaterNode Dashboard" in response.data


def test_dashboard_serves_chartjs_locally(client):
    """
    Chart.js used to come from a CDN, so the dashboard broke on any network
    that blocked it. It's vendored now, and must actually be served.
    """
    html = client.get("/").data.decode()
    assert "cdnjs.cloudflare.com" not in html

    response = client.get("/static/vendor/chart.umd.js")
    assert response.status_code == 200
    assert len(response.data) > 100_000


def test_dashboard_page_survives_chartjs_not_loading(client):
    """
    Chart.js is a CDN dependency. It used to be called at the top level, so if
    it failed to load the ReferenceError killed the whole script and every
    value on the page stayed blank.
    """
    html = client.get("/").data.decode()
    assert "typeof Chart !== \"undefined\"" in html
    assert "if (!chartsAvailable) return;" in html


def test_dashboard_page_reports_fetch_failures(client):
    """An unexplained empty dashboard looks the same as having no data."""
    assert "could not load data" in client.get("/").data.decode()


def test_dashboard_page_omits_the_api_key_when_auth_is_disabled(client):
    """With auth off there's no key to embed, so it shouldn't leak into the HTML."""
    assert config.API_KEY.encode() not in client.get("/").data


# --- error handlers ---

def test_unknown_route_returns_json_not_html(client):
    response = client.get("/api/does-not-exist")
    assert response.status_code == 404
    assert response.get_json()["error"] == "Not found"
