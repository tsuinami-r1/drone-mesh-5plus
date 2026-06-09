# drone-mesh-5plus — Claude Code Instructions

## Pre-commit checklist (required before every commit)

1. `python3 -m py_compile mesh-mapper.py` — must exit 0
2. Review changed C++ for: correct board conditional coverage (S3 **and** C5 paths),
   correct GPIO numbers, no hardcoded array sizes that should use a `#define` constant
3. Check that any new detection type cannot bleed into OpenDroneID-specific code paths
   (popup logic, FAA lookup, isNoGpsDrone, etc.)

## Deployment context

- **Scale**: multi-node, city-wide mesh
- **Hardware mix**: XIAO ESP32-S3 and XIAO ESP32-C5 nodes running simultaneously
- All firmware changes must compile and behave correctly for **both** board targets
- Node naming convention: `NODE_ID` in firmware (e.g. "RX01", "RX02") must match the
  Meshtastic shortName/longName on the paired Heltec node for GPS position resolution
- `_fetch_meshtastic_position` matches by name first, falls back to first GPS node —
  correct Meshtastic node naming is required for accurate range rings in dense meshes

## Architecture notes

- `mesh-mapper.py`: Flask + SocketIO server; detection flow is
  `serial_reader` → `update_detection()` → `socketio.emit` → browser JS
- `_skip_faa` is an internal routing flag; it must be `.pop()`-ed at the top of
  `update_detection()` and must never appear in emitted JSON
- `NODE_LOCATIONS` and `MESHTASTIC_URLS` share `NODE_LOCATIONS_LOCK`
- Analog FM detections (type == "analog_fm") have no drone/pilot GPS;
  they render as range rings + 📡 markers via `analogFmRings` / `analogFmMarkers`
- `isNoGpsDrone` must always carry `&& det.type !== 'analog_fm'` guard

## RX5808 firmware

- Source: `rx5808-detection/src/`
- Board pin selection: `#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(ARDUINO_XIAO_ESP32C5)`
- `FPV_CHANNEL_COUNT` is a `#define` (not `extern const int`) so it can size stack arrays;
  enforced at build time by `static_assert` in `rx5808.cpp`
- ADC attenuation is **per-pin** (`analogSetPinAttenuation`) not global
