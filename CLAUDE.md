# drone-mesh-5plus — Claude Code Instructions

## Branch layout (station tiers)

- **`main` (this branch)**: Level 2 stations (Wi-Fi/BLE Remote ID, DJI DroneID,
  MAVLink firmware), the home node bridge, and the **only copy of `mesh-mapper.py`**
- **`level1-station`**: Level 1 stations (analog FPV receivers: `level1-analog-fpv/`,
  RX5808 today, RX3364 planned). Firmware only; it never carries the mapper. The
  station is the v2 design: four sector patches behind an SP4T switch (bearing) and a
  video sync separator (video check + fingerprint)
- Both tiers feed `mesh-mapper.py`. The Level 1 side of that interface is the
  "Level 1 station contract" table in README.md; a change to the keys the mapper
  reads from `type == "analog_fm"` detections must be mirrored in the
  `level1-station` README and firmware in the same change set
- Never re-add Level 1 firmware here; never copy `mesh-mapper.py` there

## Pre-commit checklist (required before every commit)

1. `python3 -m py_compile mesh-mapper.py` — must exit 0
2. Review changed C++ for: correct board conditional coverage (S3 **and** C5 paths),
   correct GPIO numbers, no hardcoded array sizes that should use a `#define` constant
3. Check that any new detection type cannot bleed into OpenDroneID-specific code paths
   (popup logic, FAA lookup, isNoGpsDrone, etc.)
4. If the mapper's handling of `analog_fm` detections changed, confirm a detection
   carrying only the keys in the Level 1 contract still renders a ring (Level 1
   stations in the field are not reflashed when the mapper updates) and run
   `python3 mapper_test/analog_fusion_test.py`

## Deployment context

- **Scale**: multi-node, city-wide mesh with Level 1 and Level 2 stations fielded
  side by side
- **Hardware mix**: XIAO ESP32-S3 and XIAO ESP32-C5 nodes running simultaneously
- All firmware changes must compile and behave correctly for **both** board targets
- Node naming convention: `NODE_ID` in Level 1 firmware (e.g. "RX01", "RX02") must
  match the Meshtastic shortName/longName on the paired Heltec node for GPS position
  resolution
- `_fetch_meshtastic_position` matches by name first, falls back to first GPS node —
  correct Meshtastic node naming is required for accurate range rings in dense meshes
- Heltec UART is always on the XIAO **D4 (TX) / D5 (RX)** header pins in every
  current firmware, both tiers, so one carrier PCB fits either

## Architecture notes

- `mesh-mapper.py`: Flask + SocketIO server; detection flow is
  `serial_reader` → `update_detection()` → `socketio.emit` → browser JS
- `_skip_faa` is an internal routing flag; it must be `.pop()`-ed at the top of
  `update_detection()` and must never appear in emitted JSON
- `NODE_LOCATIONS` and `MESHTASTIC_URLS` share `NODE_LOCATIONS_LOCK`
- Analog FM detections (type == "analog_fm") come from Level 1 stations, have no
  drone/pilot GPS, and render as range rings + 📡 markers via `analogFmRings` /
  `analogFmMarkers`
- `isNoGpsDrone` must always carry `&& det.type !== 'analog_fm'` guard
- Analog range/colour run on dBm: `_analog_normalise()` attaches `rssi_dbm` (firmware
  value, else `_rssi_raw_to_dbm` RX5808 fallback: 0–1320 counts ≈ −95…−20 dBm) and
  `_max_range_m()` uses `_fspl_const_db(freq_mhz)`; `rfRssiToColor(det)` colours on
  −60/−75 dBm. Never reintroduce raw-count assumptions: counts differ between the
  S3 and C5 ADCs
- Multi-station fusion: `ANALOG_OBS` (per mac, per node) → `_analog_refresh_fixes()`
  (frequency clustering, `_analog_solve()` grid search with closed-form TX power,
  silent-station penalties, mirror-basin detection) → `ANALOG_FIXES`, `analog_fix`
  socket event, `/api/analog_fixes`, `_tak_enqueue_fix()`. `NODE_STATUS` (own lock)
  holds liveness + `threshold_dbm` from heartbeats; the serial reader must call
  `_analog_note_node()` on a Level 1 heartbeat *before* dropping it
- Level 1 v2 stations also emit `hw`, `sectors`, `sector`, `bearing_deg`,
  `bearing_sigma_deg`, `freq_peak`, `video`, `sync_hz`, `field_hz`, `sync_q`, `fp`.
  These are stored and re-emitted untouched today; using bearings as a solver residual
  and `fp` as a clustering key is the next change here
- The XIAO ESP32-C5 D4/D5 GPIO numbers are disputed: `remoteid-c5-5g` hardcodes
  GPIO6/GPIO7, the Arduino variant says GPIO23/GPIO24 (and calls GPIO6 the battery
  sense pin). If the variant is right, fielded C5 Level 2 nodes have a silent mesh
  relay. See the warning under "Wiring for mesh integration" in README.md; settle it
  with a continuity test before changing either side
- Run `python3 mapper_test/analog_fusion_test.py` after touching anything analog
- Home node (`node-mode-dualcore/src/main_home.cpp`) forwards `"analog_fm"` lines
  without MAC dedup: several stations legitimately report the same synthetic MAC
