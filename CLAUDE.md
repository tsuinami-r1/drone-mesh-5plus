# drone-mesh-5plus — Claude Code Instructions

## Branch layout (station tiers)

- **`main` (this branch)**: Level 2 stations (Wi-Fi/BLE Remote ID, DJI DroneID,
  MAVLink firmware), the home node bridge, and the **only copy of `mesh-mapper.py`**
- **`level1-station`**: Level 1 stations (analog FPV receivers: `rx5808-detection/`
  today, RX3364 planned). Firmware only; it never carries the mapper
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
   stations in the field are not reflashed when the mapper updates)

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
- Range estimation (`_rssi_raw_to_dbm`, `_FSPL_5800_DB`) and ring colour thresholds
  (`rfRssiToColor`) currently assume RX5808 ADC counts and 5.8 GHz; the RX3364 plan on
  `level1-station` (`docs/RX3364-INTEGRATION-PLAN.md`) specifies the additive
  `receiver` / `rssi_dbm` keys and the frequency-derived FSPL that replace those
  assumptions without breaking existing stations
