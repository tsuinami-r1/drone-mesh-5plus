# drone-mesh-5plus / `level1-station` — Claude Code Instructions

This branch holds **Level 1 station firmware only** (analog FPV receivers: RX5808
today, RX3364 planned). `mesh-mapper.py`, the Level 2 (Wi-Fi/BLE Remote ID)
firmware and the home node live on `main` and must not be copied here.

The station is the **v2 design**: four sector patches behind an SP4T switch
(bearing), a sync separator on the receiver's video output (video check +
fingerprint), one RX5808. There is no omni-antenna "v1" build any more.

## Pre-commit checklist (required before every commit)

1. `cd level1-analog-fpv && pio run -e seeed_xiao_esp32s3 -e seeed_xiao_esp32c5`
   must succeed for **both** boards with no warnings from `src/`
2. Review changed C++ for: pins taken from `config.h` only (never a bare GPIO number),
   no hardcoded array sizes that should use `RX_CHANNEL_COUNT` / `SECTOR_COUNT`,
   every mesh line provably ≤ `MESH_LINE_MAX` (see the length budget below)
3. If any emitted JSON key changed (see "Mapper contract" in README.md), the same
   commit series must carry the matching `mesh-mapper.py` change on `main`; note the
   `main` commit in the commit message
4. If the emitted JSON changed, rebuild `firmware/*.bin` from the default configuration
5. Never add mapper, Level 2 or home-node code to this branch

## Deployment context

- **Scale**: multi-node, city-wide mesh with Level 1 and Level 2 stations fielded
  side by side; Level 1 stations are parked densely because fix accuracy is a
  fraction of station spacing
- **Hardware mix**: XIAO ESP32-S3 and XIAO ESP32-C5 running simultaneously; every
  firmware change must compile and behave correctly for **both** board targets
- `NODE_ID` must match the Meshtastic shortName/longName on the paired Heltec node;
  the mapper resolves the station position by that name first and falls back to the
  first GPS node, so a wrong name draws everything in the wrong place. Keep it ≤ 6
  characters: every extra character costs one byte of mesh-line headroom
- `STATION_HEADING_DEG` is the true bearing of the N face and is added to every
  reported bearing; it is measured at install time
- Heltec UART is always on the XIAO **D4 (TX) / D5 (RX)** header pins, same as the
  Level 2 firmware, so one carrier PCB fits either tier

## Pins

- All pins are `PIN_*` macros in `config.h` that resolve to the board variant's
  `D0..D10` symbols (`pins_arduino.h`), so there is no per-board `#if` for pins and
  GPIO numbers follow the board's own definition. A non-XIAO board fails with `#error`
- Pin roles: D0 RSSI (ADC), D1/D2/D3 switch V1/V2/V3, D4/D5 UART, D6 CSYNC, D7 VSYNC,
  D8 CLK, D9 CS, D10 DATA. Resolved GPIOs are tabulated in `config.h` and README
- **C5 caveat**: the variant maps D0=GPIO1, D4=GPIO23, D5=GPIO24, D6=GPIO11, D7=GPIO12.
  Earlier firmware on this branch hard-coded D0=GPIO2, D4=GPIO6, D5=GPIO7 for the C5,
  which disagrees. The variant is trusted (its D6/D7 are the C5's UART0 defaults, as
  the S3's are) pending a continuity test on the first C5 v2 board. Do not "fix" the
  variant numbers back without that test

## Mapper contract (interface to `main`)

- Detections are `type: "analog_fm"` JSON lines; the mapper keys every analog-FM
  code path off that literal
- `mac` is synthetic, `AF:00:` + freq hi/lo + band ASCII + channel; it is the
  tracking key, so it must be stable per channel and unique across receivers
- `rssi_dbm` (calibrated in firmware from `rssi_mv` via the `RSSI_CAL_*` line, on the
  strongest sector) is what the mapper uses for ring radius, ring colour (−60/−75 dBm)
  and the multi-station position solver; `rssi_raw` is only the mapper's fallback.
  Raw counts are not comparable between an S3 and a C5
- v2 keys `sectors`, `sector`, `bearing_deg`, `bearing_sigma_deg`, `freq_peak`,
  `video`, `sync_hz`, `field_hz`, `sync_q`, `fp` are additive. The mapper on `main`
  ignores them today; teaching the solver bearings and `fp` is the next `main` change
- **Mesh line length budget**: `MESH_LINE_MAX` = 200 bytes. The compact detection
  line carries type/mac/freq/band/ch/rssi_dbm + bearing pair + (`fp` or
  `"video":"none"`) + node_id/seq, worst case 191 bytes with a 4-char `NODE_ID`.
  `emit_detection` drops the fingerprint, then the bearing, if a line would exceed
  the limit; it never truncates JSON. `seq` is 16-bit for the same reason. Re-run the
  worst-case count whenever a mesh key is added
- The mesh line **must be JSON**: the home node forwards JSON unchanged (analog_fm
  bypasses its MAC dedup) and tags anything else `[MESH]`, which the mapper drops
- Heartbeat/info lines must carry `heartbeat`, `status` or `info` and none of
  `mac`/`drone_lat`/`pilot_lat`/`basic_id`/`remote_id`, or the mapper treats them as a
  detection. They must carry `node_id` and `threshold_dbm`: the mapper records the
  station as alive and uses silence as a "not within range" constraint. USB and mesh
  heartbeats have different key sets (mesh is the subset the mapper stores)
- `PEAK_PICK` reports only the strongest of adjacent channels so one VTX is one
  tracking key per station; the mapper additionally clusters reports within 20 MHz

## Firmware layout (`level1-analog-fpv/`)

- `config.h` — every pin and tuneable; nothing else defines a constant
- `main.cpp` — sweep (every channel × every sector), peak-pick, fine-tune + video on
  the `VIDEO_MAX_PER_SWEEP` strongest due peaks, JSON, relay, heartbeat. Receiver-agnostic:
  refers only to `RX_*` names
- `analog_receiver.h` — the interface; `-DRECEIVER_RX5808` in `platformio.ini` selects
  `receivers/rx5808.*`. New receivers (RX3364) go behind this seam per
  `docs/RX3364-INTEGRATION-PLAN.md`; do not fork `main.cpp` per receiver
- `sector_switch.*` — `SECTOR_SWITCH_TABLE` drives V1/V2/V3; `SECTOR_AZIMUTHS`
- `bearing.*` — strongest sector + `BEARING_DEG_PER_DB × (right − left)`, sigma widened
  for weak peaks, noise-floor neighbours, rear sector within 3 dB
- `video_sync.*` — GPIO-interrupt edge counting on CSYNC/VSYNC for `VIDEO_MEASURE_MS`;
  standard decided by **field** rate (50/60 ± 3), line rate only has to sit in
  14.5–17 kHz because an LM1881's CSYNC carries equalising pulses. Inputs are
  `INPUT_PULLDOWN` so an unpopulated separator reads `none` instead of noise

## RX5808 driver notes

- Synthesizer register B is a split N/A field (`((tf/32)<<7) | (tf%32)`), not the flat
  `(freq-479)/2`; the flat form mistunes by ~4 GHz
- ADC attenuation is **per-pin** (`analogSetPinAttenuation`) not global
- RSSI reads use `analogRead` (raw, for the threshold) **and** `analogReadMilliVolts`
  (eFuse-calibrated, for dBm); JSON is built with `snprintf`, no ArduinoJson
- Mesh relay lines are `\n`-terminated only (no CR) and written as one burst

## RX3364 work

- Follow `docs/RX3364-INTEGRATION-PLAN.md`; gate 0 (bench characterisation of the
  tuning interface and RSSI curve) must be recorded there before driver code lands
- The `analog_receiver.h` seam and `receivers/` layout the plan describes already exist
