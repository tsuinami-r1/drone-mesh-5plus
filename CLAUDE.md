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
- `rssi_dbm` (calibrated in firmware from `rssi_mv` via the `RSSI_CAL_*` line) is what
  the mapper uses for the ring radius, ring colour (−60/−75 dBm) and the
  multi-station position solver; `rssi_raw` is only the mapper's fallback for
  pre-`rssi_dbm` stations (RX5808 curve, 0–1320 counts). Raw counts are not comparable
  between an S3 and a C5; never make the mapper depend on them again
- `rssi_min`/`rssi_max`/`rssi_n`/`seq`/`receiver`/`rssi_mv` are additive; the mesh line
  drops `rssi`, `basic_id`, `receiver`, `rssi_mv`, `rssi_n` and the mapper backfills them
- The mesh relay line **must be JSON** and ≤ ~190 bytes: the home node forwards JSON
  unchanged (analog_fm bypasses its MAC dedup) and tags anything else `[MESH]`, which
  the mapper drops. Rate-limit it with `MESH_REPORT_INTERVAL_MS`; LoRa airtime is the
  scarce resource in a dense deployment
- Heartbeat/info lines must carry `heartbeat`, `status` or `info` and none of
  `mac`/`drone_lat`/`pilot_lat`/`basic_id`/`remote_id`, or the mapper treats them as a
  detection. They must carry `node_id` and `threshold_dbm`: the mapper records the
  station as alive and uses silence as a "not within range" constraint
- `PEAK_PICK` reports only the strongest of adjacent channels so one VTX is one
  tracking key per station; the mapper additionally clusters reports within 20 MHz

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
- RSSI reads use `analogRead` (raw, for the threshold) **and** `analogReadMilliVolts`
  (eFuse-calibrated, for dBm); JSON is built with `snprintf`, no ArduinoJson
- `firmware/*.bin` are rebuilt from the default configuration whenever the emitted
  JSON changes (`pio run -e seeed_xiao_esp32s3 -e seeed_xiao_esp32c5`)

## Station v2 hardware

- `docs/LEVEL1-V2-HARDWARE.md` is the spec: 4 sector patches + SP4T switch on
  D1/D2/D3, video sync separator on D6/D7, pin budget for S3 and C5. New JSON keys it
  introduces (`bearing_deg`, `sectors`, `video`, `sync_hz`) are additive and need the
  matching mapper change on `main` before they are emitted

## RX3364 work

- Follow `docs/RX3364-INTEGRATION-PLAN.md`; gate 0 (bench characterisation of the
  tuning interface and RSSI curve) must be recorded there before driver code lands
- New receivers go behind the shared `AnalogReceiver` interface described in the
  plan; do not fork `main.cpp` per receiver
