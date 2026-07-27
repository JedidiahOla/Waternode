# demo_alert.py. Drives the demo database into a CONFIRMED alert
#
# seed_db.py alone leaves the dashboard sitting at NONE, which shows the UI
# works but not what the detector is for. This runs the seeded readings
# through CUSUM to clear the warmup window, then appends one contamination
# event so the dashboard shows a live alert.
#
# The event is invented. It is NOT a real measurement, and neither is the
# baseline it sits on.
#
# Run after seed_db.py:
#   python seed_db.py
#   python demo_alert.py
#   python app.py

import time

import algorithm
import config
import database as db

NODE_ID = "WN001"

# Contamination event. Turbidity well past the WHO limit and TDS sharply up,
# which is roughly what surface runoff into a source looks like.
EVENT_TURBIDITY_NTU = 180.0
EVENT_TDS_MGL = 420.0
EVENT_TEMP_C = 13.1
EVENT_BATTERY_MV = 3880


def warm_up_cusum() -> int:
    """
    Replay the readings already in the database through the detector.

    seed_db.py writes rows straight to SQLite without running CUSUM, so the
    accumulators start at zero and everything sits inside the warmup window.
    Replaying gets the state to where it would be after a node had really
    been reporting for a day.
    """
    readings = db.get_recent_readings(NODE_ID, hours=48)
    if len(readings) <= config.CUSUM_WARMUP_READINGS:
        return len(readings)

    algorithm.process_batch(readings)
    return len(readings)


def inject_event() -> dict:
    """Append one anomalous reading and run it through the detector."""
    reading = {
        "ts": int(time.time()) - 300,      # 5 min ago, so it lands on the chart
        "node_id": NODE_ID,
        "tb": EVENT_TURBIDITY_NTU,
        "tbr": int(EVENT_TURBIDITY_NTU * 800),
        "td": EVENT_TDS_MGL,
        "tc": EVENT_TEMP_C,
        "bv": EVENT_BATTERY_MV,
        "al": 2,        # the node's own local threshold check would have tripped
        "ff": 0,
        "si": 0,
    }
    db.insert_readings([reading])
    return algorithm.process_batch([reading])[0]


def main():
    db.init_db()

    replayed = warm_up_cusum()
    if replayed <= config.CUSUM_WARMUP_READINGS:
        print(f"[demo_alert] Only {replayed} readings in {config.DATABASE_PATH}, "
              f"need more than {config.CUSUM_WARMUP_READINGS} to clear warmup.")
        print("[demo_alert] Run: python seed_db.py")
        return

    result = inject_event()
    labels = ["NONE", "WATCH", "WARNING", "CONFIRMED"]
    level = result.get("backend_alert_level", 0)

    print(f"[demo_alert] Replayed {replayed} readings through CUSUM.")
    print(f"[demo_alert] Injected event: {EVENT_TURBIDITY_NTU} NTU, "
          f"{EVENT_TDS_MGL} mg/L")
    print(f"[demo_alert] Alert level is now {labels[level]}, "
          f"triggered by {', '.join(result.get('confirmed_alarms')) or 'none'}")
    print("[demo_alert] This is a synthetic event, not a real measurement.")
    print("[demo_alert] Start the app to see it: python app.py")


if __name__ == "__main__":
    main()
