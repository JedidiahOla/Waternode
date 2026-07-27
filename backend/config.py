# WATERNODE BACKEND - CONFIGURATION
# Flask + SQLite + CUSUM Early Warning System
# Dublin City University - Final Year Project

import os
from pathlib import Path

from dotenv import load_dotenv

# Load .env from this file's directory, so it works regardless of cwd.
# Does not override variables already set in the environment, which keeps
# real deployment env vars (and the test suite's) authoritative.
load_dotenv(Path(__file__).resolve().parent / ".env")

# SERVER
DEBUG = os.getenv("DEBUG", "false").lower() == "true"
PORT = int(os.getenv("PORT", 5000))
DATABASE_PATH = os.getenv("DATABASE_PATH", "waternode.db")


def _require_secret(env_var: str, dev_fallback: str) -> str:
    """
    Read a secret from the environment. DEBUG falls back to a fake dev value
    with a warning; otherwise refuse to start rather than use a guessable one.
    """
    value = os.getenv(env_var)
    if value:
        return value
    if DEBUG:
        print(f"[config] WARNING: {env_var} not set, using an insecure "
              f"dev-only default. Do not deploy like this.")
        return dev_fallback
    raise RuntimeError(
        f"{env_var} environment variable must be set when DEBUG=false. "
        f"See .env.example."
    )


# SECRETS
# Never commit real values for these, set them via environment variables
# (or a local .env file, which is gitignored). See .env.example.
SECRET_KEY = _require_secret("SECRET_KEY", "dev-only-insecure-secret-key")

# Shared API key, must match the firmware's config.h API_KEY.
# Sent in the request body (not a header) for /api/readings because the
# SIM800L's AT+HTTPDATA command path can't set custom HTTP headers.
# Used as a header (x-api-key) or query param for the read-only dashboard
# endpoints below.
API_KEY = _require_secret("API_KEY", "dev-only-insecure-api-key")

# Set this to False to make the dashboard JSON endpoints (/api/latest,
# /api/history, /api/alerts) publicly readable with no key at all. Off by
# default. See README "Known limitations" for the auth model's caveats.
DASHBOARD_REQUIRE_AUTH = os.getenv("DASHBOARD_REQUIRE_AUTH", "true").lower() == "true"

# SMS ALERTS. Twilio
# Sign up at twilio.com for a free trial. Set these as environment variables,
# never hardcode credentials in source.
TWILIO_ACCOUNT_SID = os.getenv("TWILIO_ACCOUNT_SID", "")
TWILIO_AUTH_TOKEN = os.getenv("TWILIO_AUTH_TOKEN", "")
TWILIO_FROM_NUMBER = os.getenv("TWILIO_FROM_NUMBER", "")    # e.g. "+15551234567"
ALERT_PHONE_NUMBER = os.getenv("ALERT_PHONE_NUMBER", "")    # Recipient number

# If Twilio credentials are empty, SMS sending is disabled (logged only)
SMS_ENABLED = all([TWILIO_ACCOUNT_SID, TWILIO_AUTH_TOKEN,
                    TWILIO_FROM_NUMBER, ALERT_PHONE_NUMBER])

# CUSUM ALGORITHM PARAMETERS
# k = slack/allowance parameter
# h = decision threshold, higher h means fewer false alarms but slower
#     detection of small/gradual shifts. Tune with ARL0 = e^(2*h*k) / (2*k^2)
#     roughly, or by simulation against your own baseline noise.
# See algorithm.py for the full CUSUM derivation.
CUSUM_K = 0.5   # Slack parameter
CUSUM_H = 8     # Decision threshold (tuned empirically against bench data;
                # raise for fewer false alarms, lower for faster detection)

# Minimum readings before CUSUM is considered reliable.
# During this period CUSUM scores are computed but alerts are suppressed.
CUSUM_WARMUP_READINGS = 50

# Consecutive CUSUM alarms required before a parameter escalates to CONFIRMED.
# See compute_alert_level() in algorithm.py for what this changes.
CUSUM_PERSISTENCE = 1

# SMS suppression: minimum minutes between backend alert SMS for same parameter
SMS_SUPPRESSION_MINUTES = 60

# SEASONAL BASELINE PARAMETERS
# Fixed CUSUM baseline (mu0, sigma0) per parameter per season. Dry row is from
# real Dublin bench measurements; the other three are WHO/literature defaults.
# Season indices: 0=Dry(Nov-Apr), 1=PreMonsoon(May-Jun),
#                 2=Monsoon(Jul-Sep),  3=PostMonsoon(Oct)
SEASONAL_BASELINES = {
    #                   [Dry,   PreMon, Monsoon, PostMon]
    "turbidity_mean":   [1.0,   8.0,    25.0,    10.0],   # NTU
    "turbidity_std":    [5.0,   5.0,    15.0,    6.0],
    "tds_mean":         [54.0,  150.0,  120.0,   160.0],  # mg/L
    "tds_std":          [40.0,  50.0,   60.0,    45.0],
    "temperature_mean": [12.5,  19.0,   22.0,    17.0],   #C
    "temperature_std":  [1.2,   3.0,    2.0,     3.0],
}
# Dry-season temperature (12.5 +/- 1.2 C) is measured Dublin mains water:
# colder and tighter than the Nepal placeholder it replaced, as expected.

SEASON_NAMES = ["Dry (Nov-Apr)", "Pre-Monsoon (May-Jun)",
                "Monsoon (Jul-Sep)", "Post-Monsoon (Oct)"]

# WHO absolute limits (source: WHO Guidelines for Drinking-Water Quality, 4th ed.)
# Must match firmware config.h WHO_TURBIDITY_LIMIT_NTU and WHO_TDS_LIMIT_MGL
WHO_TURBIDITY_NTU = 4.0     # WHO guideline value
WHO_TDS_MGL = 900.0         # Above palatability threshold (matches firmware)

# DASHBOARD
DASHBOARD_HOURS = 24
DASHBOARD_POLL_INTERVAL_SEC = 5    # Frontend polling interval

# DATA RETENTION
# Archive readings older than this many days to keep DB lean
# (All data preserved in CSV on SD card regardless)
DB_RETENTION_DAYS = 90
