# WaterNode Backend

Flask + SQLite backend for a low-cost IoT water-quality early-warning system,
originally built as a final year Electronic & Computer Engineering project
(Dublin City University). An ESP32 sensor node measures turbidity, TDS, and
temperature and POSTs batched readings to this backend over GPRS. The backend
runs a CUSUM (cumulative sum) anomaly detector against a seasonal reference
baseline, fuses all three parameters with a Mahalanobis distance score for
compound-event detection, and sends an SMS via Twilio when an alert is
confirmed.

The ESP32 firmware (C++/Arduino) that talks to this backend lives in
[`../firmware`](../firmware). See the [top-level README](../README.md) for how
the two halves fit together and why the detector is built this way.

**Status:** bench prototype, never physically deployed. Built and tested
against Dublin tap water. The firmware's Nepal-specific values (GPRS APN,
timezone, most of the seasonal baseline table) describe the hypothetical
field deployment the system was designed for, not a real site, see
"Known limitations" below for exactly which numbers here are real bench
measurements versus untouched literature defaults.

<!-- TODO: replace this comment with the screenshot line below.
     ![Dashboard](../docs/dashboard.png)
     Generate it with:
       python seed_db.py && python demo_alert.py && python app.py -->

## How detection works

Each reading is converted to a z-score against a fixed seasonal mean/std
(`config.SEASONAL_BASELINES`), then fed into a CUSUM accumulator per
parameter:

```
S+(t) = max(0, S+(t-1) + z(t) - k)
S-(t) = max(0, S-(t-1) - z(t) - k)
```

An alarm fires when either accumulator crosses the decision threshold `h`.
This catches both sudden spikes (one big z-score) and slow drift (many small
z-scores that individually wouldn't trip a fixed threshold). A Mahalanobis
distance across all currently-valid parameters (`D^2 = sum(z^2)` under an
independence assumption) escalates faster when multiple parameters move
together, e.g. a monsoon runoff event raising both turbidity and TDS at
once.

Alert levels: `NONE -> WATCH -> WARNING -> CONFIRMED`. `CONFIRMED` triggers an
SMS via Twilio (rate-limited per parameter by `SMS_SUPPRESSION_MINUTES`).

## Repo structure

```
app.py              Flask routes: ingestion API, dashboard API, dashboard page
algorithm.py         CUSUM + Mahalanobis detection logic (pure functions)
database.py          SQLite schema and all queries
notifications.py     Twilio SMS wrapper
config.py             All tunable constants and environment-variable handling
seed_db.py            Generates 24h of synthetic demo data for the dashboard
demo_alert.py         Drives that demo data into a CONFIRMED alert
templates/dashboard.html   Live-updating dashboard (vanilla JS + Chart.js)
static/vendor/        Chart.js, vendored so the dashboard works offline
tests/                pytest suite (92 tests)
```

## Setup

Run from this `backend/` directory:

```bash
python3 -m venv .venv
source .venv/bin/activate        # .venv\Scripts\activate on Windows
pip install -r requirements-dev.txt

cp .env.example .env
# edit .env, at minimum set SECRET_KEY and API_KEY
# generate one with: python -c "import secrets; print(secrets.token_hex(32))"

python seed_db.py                # optional: populate 24h of demo data
python demo_alert.py              # optional: push it into an alert state
python app.py                     # runs on http://localhost:5000
```

Twilio credentials are optional, leave them blank in `.env` and SMS sends
are logged instead of actually sent.

## Running the tests

```bash
pytest
```

92 tests, split across four files:

- **`test_algorithm.py`**, the CUSUM accumulator (noise filtering below `k`,
  threshold crossing, reset-on-alarm, consecutive-alarm decay), the
  Mahalanobis fusion including degrees-of-freedom handling when a sensor is
  faulted, seasonal baseline lookup, and the shared alert-level logic.
- **`test_database.py`**, dedup on insert, CUSUM state round-trip through
  SQLite, alert log.
- **`test_api.py`**, the HTTP surface: auth on every route, plus malformed
  input. The node POSTs to a public URL, so anything that finds the address
  can hit these routes. Each case asserts a 4xx rather than a 500.
- **`test_demo_scripts.py`**, that `seed_db.py` and `demo_alert.py` still do
  what the README says. Retuning `CUSUM_H` or the warmup window can quietly
  stop the demo reaching `CONFIRMED`, and that should break a test rather than
  break for someone following the setup steps.

Tests run against a throwaway SQLite file in a temp directory, so the suite
doesn't touch a real `waternode.db` or need `.env`.

## API

| Route | Method | Auth | Purpose |
|---|---|---|---|
| `/api/readings` | POST | `api_key` in JSON body | Ingest a batch of readings from the sensor node |
| `/api/latest` | GET | `x-api-key` header or `?api_key=` | Latest reading + CUSUM state + alert level |
| `/api/history` | GET | same | Time-series data for the dashboard charts |
| `/api/alerts` | GET | same | Recent alert log |
| `/` , `/dashboard` | GET | none | Dashboard page |
| `/health` | GET | none | Health check |

The ingestion endpoint checks the API key in the request body rather than a
header, because the firmware talks to it via the SIM800L's `AT+HTTPDATA`
command, which can't easily set custom HTTP headers.

## Input validation

`/api/readings` rejects the whole batch with a 400 if any record isn't an
object, is missing a timestamp, has a non-numeric sensor value, or has a value
outside the plausible range in `NUMERIC_FIELD_RANGES`. Batches are capped at
`MAX_BATCH_SIZE`.

Rejection is all-or-nothing rather than per-record. The node re-sends its whole
buffer next cycle and inserts are idempotent on `(node_id, timestamp)`, so
dropping a bad batch beats working out which half of it landed.

API keys use `hmac.compare_digest` (constant time).

## Known limitations

This is a research prototype, not a production system. In particular:

- **Auth is a single shared secret**, not per-user authentication. Adequate
  for a controlled deployment, not for a public-facing service. Set
  `DASHBOARD_REQUIRE_AUTH=false` to disable it entirely for a local demo.
- **HTTP, not HTTPS**, between firmware and backend. The SIM800L supports TLS
  but it wasn't implemented in this iteration.
- **Single SQLite file, no replication.** Fine at this scale (one or a
  handful of nodes); wouldn't scale past that without a real database server.
- **Only the Dry-season row of `SEASONAL_BASELINES` is real bench data**
  (Dublin tap water, measured during testing). Pre-Monsoon/Monsoon/Post-Monsoon
  are still the firmware's original WHO/literature-derived placeholders,
  carried over unvalidated, recalibrate all four seasons from real
  field data before deploying anywhere.
- **`CUSUM_PERSISTENCE=1`** means `WATCH` is otherwise only reachable via the
  multivariate path or during warmup, see `compute_alert_level()` in
  `algorithm.py` for why, and what raising it changes.
- **The first 50 readings per parameter are a warmup window.** Accumulators
  run from reading one, but alarms during that window cap at `WATCH` and never
  send SMS. At 15-minute sampling that's about the first 12 hours after a node
  is commissioned.

## License

MIT. See [`../LICENSE`](../LICENSE).
