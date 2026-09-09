# drone-mesh-5plus / `level1-station` — Claude Code Instructions

This branch holds **Level 1 station firmware only** (analog FPV receivers: RX5808
today, RX3364 planned). `mesh-mapper.py`, the Level 2 (Wi-Fi/BLE Remote ID)
firmware and the home node live on `main` and must not be copied here.

## Pre-commit checklist (required before every commit)

1. Review changed C++ for: correct board conditional coverage (S3 **and** C5 paths),
   correct GPIO numbers, no hardcoded array sizes that should use a `#define` constant
2. If any emitted JSON key changed (see "Mapper contract" in README.md), the same
   commit series must carry the matching `mesh-mapper.py` change on `main`; note the
   `main` commit in the commit message
3. Never add mapper, Level 2 or home-node code to this branch

## Deployment context

- **Scale**: multi-node, city-wide mesh with Level 1 and Level 2 stations fielded
  side by side
- **Hardware mix**: XIAO ESP32-S3 and XIAO ESP32-C5 running simultaneously; every
  firmware change must compile and behave correctly for **both** board targets
- Node naming convention: `NODE_ID` in firmware (e.g. "RX01", "RX02") must match the
  Meshtastic shortName/longName on the paired Heltec node; the mapper resolves the
  station position by that name first and falls back to the first GPS node, so a
  wrong name draws the range ring in the wrong place
- Heltec UART is always on the XIAO **D4 (TX) / D5 (RX)** header pins, same as the
  Level 2 firmware, so one carrier PCB fits either tier

## Mapper contract (interface to `main`)

- Detections are `type: "analog_fm"` JSON lines; the mapper keys every analog-FM
  code path off that literal (no FAA lookup, no drone/pilot markers, 30 s stale
  timeout, range ring + 📡 marker, `ANALOGFM-` CoT uids)
- `mac` is synthetic, `AF:00:` + freq hi/lo + band ASCII + channel; it is the
  tracking key, so it must be stable per channel and unique across receivers
- `rssi_raw` drives range estimation and ring colour on the mapper with RX5808
  ADC-count assumptions (0–1320 range, colour steps 800/1000); a new receiver with a
  different RSSI curve needs the contract additions in
  `docs/RX3364-INTEGRATION-PLAN.md` (`receiver`, `rssi_dbm`) rather than reusing
  those thresholds
- Heartbeat/info lines must carry `heartbeat`, `status` or `info` and none of
  `mac`/`drone_lat`/`pilot_lat`/`basic_id`/`remote_id`, or the mapper treats them as a
  detection

## RX5808 firmware

- Source: `rx5808-detection/src/`
- Board pin selection: `#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(ARDUINO_XIAO_ESP32C5)`
- `FPV_CHANNEL_COUNT` is a `#define` (not `extern const int`) so it can size stack arrays;
  enforced at build time by `static_assert` in `rx5808.cpp`
- ADC attenuation is **per-pin** (`analogSetPinAttenuation`) not global
- Synthesizer register B is a split N/A field (`((tf/32)<<7) | (tf%32)`), not the flat
  `(freq-479)/2`; the flat form mistunes by ~4 GHz
- Mesh relay lines are `\n`-terminated only (no CR) and written as one burst; the
  Meshtastic TEXTMSG serial module broadcasts raw chunks

## RX3364 work

- Follow `docs/RX3364-INTEGRATION-PLAN.md`; gate 0 (bench characterisation of the
  tuning interface and RSSI curve) must be recorded there before driver code lands
- New receivers go behind the shared `AnalogReceiver` interface described in the
  plan; do not fork `main.cpp` per receiver
