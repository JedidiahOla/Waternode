# seed_db.py. Demo database seeder
#
# Populates the last 24 hours with synthetic, clean-baseline readings so the
# dashboard looks populated for a demo instead of showing an empty chart.
# This is NOT real sensor data, it's generated noise around the values in
# config.SEASONAL_BASELINES, for demo/UI purposes only.
#
# Run once before a demo: python seed_db.py

import math
import random
import time

import config
import database as db

NODE_ID = "WN001"

# Seed values, tuned to roughly match this project's own bench measurements
# (clean Dublin tap water), so there's no visible discontinuity when live
# readings start coming in. Adjust if your bench readings differ.
TURB_BASE = 0.5    # NTU
TDS_BASE = 52.0    # mg/L
TEMP_BASE = 12.5   #C, matches config.SEASONAL_BASELINES Dry-season mean
BATT_BASE = 3950   # mV

HOURS_BACK = 24
INTERVAL_MINUTES = 15   # one reading every 15 min = 96 readings total


def gentle_noise(base: float, spread: float,
                  lo: float | None = None, hi: float | None = None) -> float:
    """Gaussian noise around base, optionally clamped."""
    v = base + random.gauss(0, spread)
    if lo is not None:
        v = max(lo, v)
    if hi is not None:
        v = min(hi, v)
    return round(v, 2)


def make_readings() -> list[dict]:
    now = int(time.time())
    start = now - HOURS_BACK * 3600
    step = INTERVAL_MINUTES * 60
    ts = start

    rows = []
    while ts <= now - step:   # stop before "now" so live readings continue naturally
        # Slow daily drift so the chart looks like real sensor noise, not flat
        drift_hours = (ts - start) / 3600
        temp_drift = 0.5 * math.sin(2 * math.pi * drift_hours / 24)

        turb = gentle_noise(TURB_BASE, 0.02, lo=0.1)
        tds = gentle_noise(TDS_BASE, 1.0, lo=50.0)
        temp = gentle_noise(TEMP_BASE + temp_drift, 0.1, lo=5.0, hi=40.0)
        batt = gentle_noise(BATT_BASE, 20, lo=3000, hi=4200)

        rows.append({
            "ts": ts,
            "node_id": NODE_ID,
            "tb": turb,
            "tbr": int(turb * 800),   # plausible raw ADS1115 count
            "td": tds,
            "tc": temp,
            "bv": int(batt),
            "al": 0,
            "ff": 0,
            "si": 0,
        })
        ts += step

    return rows


def seed():
    db.init_db()
    rows = make_readings()
    inserted = db.insert_readings(rows)
    print(f"[seed_db] Inserted {inserted} of {len(rows)} synthetic readings "
          f"({HOURS_BACK}h, every {INTERVAL_MINUTES} min) into "
          f"{config.DATABASE_PATH}")
    print("[seed_db] This is demo data, not real sensor output.")
    print("[seed_db] Start the app and any live firmware; real readings "
          "will append naturally after these.")


if __name__ == "__main__":
    seed()
