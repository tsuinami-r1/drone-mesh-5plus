# drone-mesh-5plus / `level1-c5phy` — Claude Code Instructions

This branch holds **Level 1 station firmware only**, two designs side by side:

- **`level1-c5phy/` — the v3 prototype.** The XIAO ESP32-C5's own 5 GHz Wi-Fi PHY
  is the analog FPV receiver: it is tuned onto the FPV channels, its raw I/Q is
  read back through PARLIO, and power, FM coherence and a software sync search
  replace the RX5808 and the sync separator. **C5-only.** Builds and links;
  **not bench validated** (README "Bench validation").
- **`level1-analog-fpv/` — the v2 station** (RX5808 + SP4T + sync separator),
  unchanged from the `level1-station` branch, builds for the XIAO S3 and C5.

`mesh-mapper.py`, the Level 2 (Wi-Fi/BLE Remote ID) firmware and the home node
live on `main` and must not be copied here.

## Pre-commit checklist (required before every commit)

1. `cd level1-c5phy && pio run -e seeed_xiao_esp32c5` must succeed with no warnings
   from `src/` (a `-Wliteral-suffix` warning from the framework's `periph_ctrl.h`
   is not ours)
2. `cd level1-analog-fpv && pio run -e seeed_xiao_esp32s3 -e seeed_xiao_esp32c5`
   must still succeed for **both** boards — v2 is the fielded design
3. Review changed C++ for: pins taken from `config.h` only, no hardcoded array sizes
   that should use `RX_CHANNEL_COUNT` / `SECTOR_COUNT` / `WINDOWS_PER_SECTOR` /
   `VIDEO_WINDOWS`, every mesh line provably ≤ `MESH_LINE_MAX`
4. If any emitted JSON key changed (see "Mapper contract" in README.md), the same
   commit series must carry the matching `mesh-mapper.py` change on `main`; note the
   `main` commit in the commit message
5. If the v3 emitted JSON changed, rebuild `firmware/level1-v3-c5phy-factory.bin`
   from the default configuration (it is the `firmware.factory.bin` PlatformIO
   produces, flashed at 0x0 after a full erase)
6. Never add mapper, Level 2 or home-node code to this branch

## Licence boundary

`level1-c5phy/` is **GPL-3.0-only** (its `LICENSE`): the PHY front end, capture
path and metrics derive from C5VRX (github.com/colonelpanichacks/c5vrx). The rest
of the repository is MIT. Never copy code from `level1-c5phy/` into
`level1-analog-fpv/` or onto `main`; shared logic goes the other way (v2 →
v3), as `sector_switch.*` and `bearing.*` did.

## v3 (`level1-c5phy/`) rules

- **C5 only, by design.** The receiver is the C5's radio; there is no S3 build
  and `config.h` `#error`s on any other board. The v2 interchangeability rule
  does not apply to this directory
- **Mesh UART stays on D4/D5** (GPIO23/24 on the C5 variant), as every station tier
- **I/Q loopback lanes** are GPIO 1, 0, 2, 7, 10, 5, 3, 4 (`IQ_LANE_GPIOS`, in PARLIO
  data order Q[9:6] then I[9:6], `IQ_LANE_DIAG` = MODEM_DIAG bits 6–9 and 16–19).
  They are the set C5VRX proved on hardware. They consume header pads D0, D1, D3,
  D10 and the GPIO2–5 back pads, which must stay unconnected. Do not move them
  without a bench check; the SP4T control lines therefore sit on D2/D8/D9
- **Undocumented PHY symbols are strong references** (`phy_set_freq`,
  `phy_force_rx_gain`, `phy_disable_agc`, `phy_rfagc_disable`, `phy_wifi_fbw_sel`,
  `phy_get_rssi`, `phy_get_noise_floor` in `libphy.a`; `phy_track_pll_deinit` in
  `libesp_phy.a`; `lmac_stop_hw_txq` in `libpp.a`). Do not make them weak: a weak
  reference does not pull an archive member, so it would silently resolve to NULL.
  The platform is pinned to pioarduino 55.03.39 for this reason; re-check the
  symbols with `nm` on the new `libphy.a` before bumping it
- `rf_start()` initialises Wi-Fi itself with `sta_disconnected_pm = false`; never
  use the Arduino `WiFi` class in this project (it leaves that PM on)
- The order in `setup()` is `rf_start()` → `iq_init()` → `rf_route_iq_lanes()`: the
  PARLIO driver reconfigures the lane GPIOs as inputs, and the second routing
  re-enables their output drivers and the MODEM_DIAG signals
- Every retune re-arms the dump engine and re-asserts AGC-off / BW40 / fixed gain
  (`rf_tune`); the closed driver touches that state
- Level = `(RF_GAIN_MAX − gain) × RF_GAIN_DB_PER_STEP + 10·log10(p_mean / RF_NOISE_POWER)`;
  gain is stepped down while `clip > RF_CLIP_MAX_PERMILLE`; a sector's level is the
  **minimum** over its windows and its coherence the median. Never report a
  level from a clipped window
- The hit rule is level **and** coherence (`DETECT_LEVEL_DB`, `DETECT_Q_PHASE_MIN`).
  The coherence gate is the channel selectivity and the Wi-Fi rejection; do not
  drop it in favour of a pure level threshold
- `rssi_raw` / `rssi` / `rssi_min` / `rssi_max` are **synthesised** from dBm through
  the inverse of the mapper's RX5808 curve so older mappers draw the right ring.
  `field_hz` is nominal (50/60) for the detected standard, not measured
- Full flash erase before flashing is part of the procedure (stale PHY
  calibration data); keep it in the README

## v2 (`level1-analog-fpv/`) rules

Unchanged from `level1-station`: both boards every time; pins are `PIN_*` macros
resolving to the variant's `D0..D10`; the S3 and C5 use the same external pins;
the C5 pin map is the variant's (D4=GPIO23, D5=GPIO24), now corroborated by
C5VRX's measured pad map. RX5808 register B is a split N/A field; ADC attenuation
is per-pin; the video classifier decides PAL/NTSC by field rate.

## Deployment context

- Multi-node, city-wide mesh with Level 1 and Level 2 stations fielded side by
  side; Level 1 stations are parked densely because fix accuracy is a fraction of
  station spacing
- `NODE_ID` must match the Meshtastic shortName/longName on the paired Heltec node;
  keep it ≤ 6 characters (mesh-line headroom)
- `STATION_HEADING_DEG` is the true bearing of the N face, measured at install

## Mapper contract (interface to `main`)

- Detections are `type: "analog_fm"` JSON lines; the mapper keys every analog-FM
  code path off that literal. v3 emits the v2 keys plus USB-only diagnostics
  (`level_db`, `gain`, `q_phase`, `cfo_khz`, `carrier`, `sync_score`,
  `video_windows`) and `receiver: "c5phy"`, `hw: "v3"`. The mapper on `main` needs
  no change: it labels the receiver by frequency and prefers `rssi_dbm`
- `mac` is synthetic, `AF:00:` + freq hi/lo + band ASCII + channel — stable per
  channel, identical between v2 and v3 so a channel is one tracking key per station
- `rssi_dbm` drives ring radius, ring colour (−60/−75 dBm) and the position solver
- **Mesh line length budget**: `MESH_LINE_MAX` = 200 bytes; the compact line is the
  v2 format byte for byte (type/mac/freq/band/ch/rssi_dbm + bearing pair + `fp` or
  `"video":"none"` + node_id/seq, worst case 191 bytes with a 4-char `NODE_ID`);
  `emit_detection` drops the fingerprint, then the bearing, never truncates
- Mesh lines **must be JSON** (the home node forwards JSON unchanged and drops
  anything else); heartbeat/info lines carry `heartbeat`/`status`/`info` and none of
  `mac`/`drone_lat`/`pilot_lat`/`basic_id`/`remote_id`; they carry `node_id` and
  `threshold_dbm`. Bench console output uses `"info":"bench..."` for that reason
- `PEAK_PICK` reports only the strongest of adjacent channels; the mapper also
  clusters reports within 20 MHz
