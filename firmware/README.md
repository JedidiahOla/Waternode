# WaterNode Firmware

ESP32 firmware for the WaterNode sensor node. Samples turbidity, TDS and
temperature on a timer, buffers to SD card, runs an offline threshold check as
an SMS failsafe, and uploads batches to a Flask backend over GPRS via a
SIM800L.

See [`../backend`](../backend) for the server-side CUSUM/Mahalanobis detection,
and the [top-level README](../README.md) for how the two halves fit together.

The node deep-sleeps between cycles. `setup()` runs the whole
sense/evaluate/store/transmit/sleep cycle on every wake and `loop()` is never
reached. Everything else in the design follows from that: sensor power is
switched through a MOSFET so it draws nothing while asleep, state that has to
survive the sleep lives in RTC memory, and the RTC keeps time so timestamps
don't depend on the network.

## Project status

Bench prototype for a DCU final-year project, built, wired, and bench-tested
in Dublin (using Dublin tap water for the sensor readings), never physically
deployed. The Nepal-specific values baked into `config.h` (GPRS APN, NTP
timezone offset, initial seasonal baselines, plausibility bounds) describe the
hypothetical target deployment this was designed for, not an actual field
site, see the [backend README](../backend/README.md) for how the seasonal
baseline table splits between literature defaults and real Dublin bench
measurements.

## Pin map

| Signal | GPIO | Notes |
|---|---|---|
| DS18B20 data | 4 | OneWire, needs 4.7k pull-up to 3V3 |
| Battery sense | 36 | Input-only ADC, via 100k/100k divider |
| I2C SDA | 21 | Shared: ADS1115 (0x48) + DS3231 (0x68) |
| I2C SCL | 22 | as above |
| SD MOSI | 23 | VSPI |
| SD MISO | 19 | VSPI |
| SD CLK | 18 | VSPI |
| SD CS | 5 | VSPI |
| SIM800L RX | 17 | ESP32 TX to module RX |
| SIM800L TX | 16 | ESP32 RX from module TX |
| SIM800L PWRKEY | 13 | Pulse LOW to power on/reset |
| Sensor power | 27 | BSS138 gate, switches the whole sensor rail |
| Status LED | 2 | Onboard |
| *(spare)* | 26 | Reserved for a pH probe |

Analogue channels on the ADS1115:

| Channel | Sensor | Conditioning |
|---|---|---|
| A0 | Turbidity (SEN0189) | 100k/100k divider, sensor swings to ~4.3V |
| A1 | TDS (SEN0244) | Direct, peaks around 2.3V |

Both sensors are read through the external ADS1115 rather than the ESP32's
internal ADC, which is noticeably nonlinear above ~3.1V and below ~0.15V.

## Timing

| Setting | Value |
|---|---|
| Sample interval, normal | 15 min |
| Sample interval, anomaly active | 5 min |
| Transmit interval | 4 h |
| Samples averaged per reading | 10, discarding 2 high and 2 low |
| Fast-mode hold after alert clears | 60 min |

## Hardware

- ESP32-WROOM-32 DevKit
- DFRobot SEN0189 turbidity sensor + DFRobot SEN0244 TDS sensor
- ADS1115 16-bit ADC (I2C)
- DS18B20 waterproof temperature probe (OneWire)
- DS3231 RTC (I2C, shares the bus with the ADS1115) for timestamps that
  survive GPRS/network outages
- SIM800L GSM/GPRS module for data upload and SMS alerts
- MicroSD card for local buffering during connectivity gaps
- MT3608 boost converter (18650 cell -> regulated 5V rail) + TP4056/DW01A
  charge management
- BSS138 MOSFET for switching sensor power off during deep sleep

## Repo structure

```
waternode/              Firmware, open waternode.ino in Arduino IDE
  waternode.ino          Entry point: full wake cycle runs in setup()
  config.h                Pin map, timing, seasonal baselines, thresholds
  sensors.h / .cpp        Turbidity + TDS (ADS1115), temperature, battery
  rtc_module.h / .cpp     DS3231 access and NTP time sync
  storage.h / .cpp        SD card CSV buffering, archiving, event logging
  connectivity.h / .cpp   SIM800L: AT commands, GPRS, HTTP POST, SMS
  alert.h / .cpp          Local threshold check, offline SMS failsafe
hardware-bringup/        Standalone diagnostic sketches used during bring-up
                          (I2C scanner, SD card init tests, SPI pin check)
```

## How it works

Each wake cycle: initialise the RTC and get a timestamp, mount the SD card,
power on the sensors and take a reading, run a local threshold check against
the seasonal baseline (turbidity/TDS over 3x the seasonal mean trips an
offline SMS, independent of the backend), append the reading to the SD
buffer, and, on the configured transmit interval, connect over GPRS and
upload the buffered batch to the backend as JSON. If the backend is
unreachable, buffered readings simply accumulate and get uploaded on the
next successful connection; nothing is lost.

Two sampling modes: 15-minute intervals normally, dropping to 5-minute
intervals once a local alert is active, for better temporal resolution
around a developing event.

## Power

Not yet measured. The design targets months on a single 18650 by keeping the
duty cycle very low: the sensors and the SIM800L are the only significant
loads, sensor power is switched off entirely between readings, and the radio
only comes up once every 4 hours rather than per reading.

Whether that target is met is an open question until someone puts a meter on
it. The numbers that need measuring are deep sleep current, active current
during a sense cycle, and peak current during a GPRS transmit (the SIM800L can
pull ~2A in bursts, which is the figure most likely to break the estimate).
Until then, treat battery life as a design intent rather than a result.

Low-battery behaviour is implemented: below 3300 mV the node flags a fault,
and below 3000 mV it stops transmitting to conserve what's left.

## Setup

Arduino IDE with the `esp32:esp32` board package. Libraries, all installed
through Library Manager at their current versions:

- `OneWire`
- `DallasTemperature`
- `RTClib` (Adafruit)
- `Adafruit ADS1X15` (pulls in `Adafruit BusIO` as a dependency)

1. Install the ESP32 board package via Boards Manager, then the libraries
   above via Library Manager.
2. Open `waternode/waternode.ino`.
3. Edit `config.h`: set `SERVER_URL` to your backend's address, `API_KEY`
   to match the backend's configured key, `SMS_NUMBER_1` to the alert
   recipient, and `APN` for your carrier. These ship as obvious placeholders.
4. Select board "ESP32 Dev Module", pick the correct serial port, and flash.
5. Open Serial Monitor at 115200 baud to watch a wake cycle.

A normal cycle looks like this:

```
========== WaterNode Boot #47 ==========
Firmware v1.0.0 | Node: WN001
Season: 0 | Unix time: 1773478502
[SENSORS] Reading...
[SENSORS] Turbidity: 1.42 NTU (raw: 1131)
[SENSORS] TDS: 58.10 mg/L
[SENSORS] Temperature: 12.44 C
[SENSORS] Battery: 3912 mV
[SENSORS] Faults: 0x00
[ALERT] Level: NONE (0)
[SLEEP] Sleeping for 15 minutes
[SLEEP] Entering deep sleep
```

On a transmit cycle you also get the `[GPRS]` block: signal quality, record
count, and whether the upload succeeded. A failed upload leaves the batch in
`buffer.csv` for the next attempt.

If the SD card or a sensor is missing, the corresponding bit shows up in the
fault byte and the node carries on rather than halting. `hardware-bringup/`
has standalone sketches for isolating each peripheral when something doesn't
come up.

## Calibration

`TURB_A`/`TURB_B`/`TURB_C` (the turbidity quadratic formula coefficients)
should be verified against bentonite clay standards at your actual supply
voltage before trusting absolute NTU values, see the comments in
`config.h` for the calibration procedure. `TDS_KVALUE` similarly needs
calibrating against a known-conductivity standard (e.g. a 342 ppm NaCl
solution), it ships at `1.0` (uncalibrated) by default.

## Status / next steps

Sensing, SD buffering and the local alert path are bench-tested. The GPRS
upload and SMS path are written but have not been run against a live SIM800L,
so treat them as unproven.

That's the next validation step. AT command timing is the part worth
bench-testing module by module with a serial passthrough sketch, rather than
trusting the whole wake cycle end to end on the first try.

Known gaps, roughly in priority order:

1. **Power consumption is unmeasured.** See above. Everything about battery
   life is currently an estimate.
2. **Fault flags set after the SD write never reach the backend.**
   `FAULT_SD_WRITE` and `FAULT_GPRS_FAIL` are set on the in-memory reading
   after it's already been appended to `buffer.csv`, so they only land in
   `system.log`. Needs writing before the append, or carrying into the next
   cycle's record.
3. **Maintenance-mode suppression** (don't alarm on tank cleaning). Designed
   but cut from this build, since there was no way to activate it, no button
   and no inbound SMS handler. The SMS handler is the obvious route in.
4. **TLS.** The SIM800L supports it; uploads currently go over plain HTTP.

## References

- Turbidity/TDS conversion formulas: DFRobot application notes for the
  SEN0189 (turbidity) and SEN0244 (TDS) sensors
- CUSUM detection: Page, E.S., "Continuous Inspection Schemes,"
  *Biometrika*, 1954
- Seasonal baseline values informed by WHO *Guidelines for Drinking-Water
  Quality* (4th ed.) and published water quality data for similar
  hill-station sources
- SIM800L power sequencing: SIMCOM SIM800L Hardware Design Guide

## Build photos

Photos of the assembled sensor node and the sensors under test, see
`photos/`.

![Assembled sensor node](photos/assembled-node.jpg)
![Sensor test setup](photos/sensor-test-setup.jpg)

## License

MIT. See [`../LICENSE`](../LICENSE).
