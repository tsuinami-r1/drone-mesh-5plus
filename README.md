# <div align="center">  **Remote Drone Mapper — Level 1 stations** </div>

<div align="center">

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ESP32](https://img.shields.io/badge/ESP32-S3%20%7C%20C5-green.svg)](https://www.espressif.com/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-pioarduino-orange.svg)](https://github.com/pioarduino/platform-espressif32)

**Analog FPV video detection stations for the drone-mesh-5plus counter-surveillance network.**

Branch `level1-station` — firmware for the RX5808 (5.8 GHz) receiver today, the
RX3364 (3.3 GHz) receiver next. Feeds `mesh-mapper.py` on `main`.

[🏗️ Station tiers](#️-station-tiers) • [⚡ Quick Start](#-quick-start) • [📡 RX5808](#-rx5808-58ghz-analog-fm-detection-rx5808-detection) • [🔌 Mapper contract](#-mapper-contract-what-a-level-1-station-emits) • [🧭 Station v2 hardware](docs/LEVEL1-V2-HARDWARE.md) • [🗺️ RX3364 roadmap](docs/RX3364-INTEGRATION-PLAN.md)

</div>

---

## 🏗️ **Station tiers**

The network fields two distinct kinds of station. They are developed on separate
branches of this repository and meet only at the collection point, where both feed
the same `mesh-mapper.py`.

| Tier | What it listens for | Hardware | Branch |
|------|---------------------|----------|--------|
| **Level 1** | Analog FPV **video carriers**: 5.8 GHz today (RX5808), 3.3 GHz planned (RX3364). No decoding, RSSI only. Produces a range ring around the station. | XIAO ESP32-S3 or C5 + analog receiver module + Heltec V3 | **`level1-station`** (this branch) |
| **Level 2** | Digital **Remote ID / DJI DroneID / MAVLink** over Wi-Fi and BLE. Decodes drone and pilot GPS. | XIAO ESP32-S3 or C5 + Heltec V3 | `main` |
| Collection point | `mesh-mapper.py`, the home node bridge, TAK output, Raspberry Pi installer | Laptop / Raspberry Pi + XIAO S3 `home_node` + Heltec V3 | `main` |

**Branch rules**

- This branch holds **only Level 1 firmware** and its docs. `mesh-mapper.py`, the
  Level 2 firmware and the home node live on `main` and are never copied here, so
  there is exactly one mapper to keep compatible.
- The interface between the tiers is the **JSON line contract** in
  [Mapper contract](#-mapper-contract-what-a-level-1-station-emits). A Level 1
  change that alters the emitted keys must land together with a matching
  `mesh-mapper.py` change on `main`.
- Level 1 stations and Level 2 stations share the same **Heltec / Meshtastic
  backhaul** and the same D4/D5 UART wiring, so one carrier PCB serves both.

---

## ⚡ **Quick Start**

### What you need

| Role | Hardware | Quantity |
|------|----------|----------|
| Level 1 station MCU | Seeed **XIAO ESP32-S3** or **XIAO ESP32-C5** | one per station |
| Receiver | **RX5808** 5.8 GHz analog FM module (RX3364 3.3 GHz: see the [roadmap](docs/RX3364-INTEGRATION-PLAN.md)) | one per station |
| Field mesh radio | **Heltec WiFi LoRa 32 V3** running Meshtastic, wired to the XIAO | one per station |
| Collection point | The `main` branch: `mesh-mapper.py`, home node, Raspberry Pi installer | 1 per network |
| Build machine | A computer with VS Code and a USB-C cable | 1 |

### Step 1 — Toolchain

Firmware is built with **PlatformIO inside VS Code via the pioarduino IDE
extension**, exactly as on `main`; follow
[Quick Start Step 1 on `main`](https://github.com/tsuinami-r1/drone-mesh-5plus/blob/main/README.md#step-1--install-the-toolchain-vs-code--pioarduino)
for the extension, udev rules and CLI alternative. Then check out this branch:

```bash
git clone --branch level1-station https://github.com/tsuinami-r1/drone-mesh-5plus.git level1-station
```

### Step 2 — Open the firmware project

**File → Open Folder…** → `rx5808-detection/` (the project folder, not the repo
root). Environments: `seeed_xiao_esp32s3`, `seeed_xiao_esp32c5`.

### Step 3 — Set the compile-time options, build, flash

Follow the [RX5808 flashing steps](#full-flashing-steps) below. The two settings
that matter in the field are `NODE_ID` (unique per station, equal to the paired
Meshtastic node name) and `RSSI_THRESHOLD` (calibrated on site).

### Step 4 — Wire to the Heltec and name the Meshtastic node

Three wires: XIAO **D4** (TX) → Heltec RX, XIAO **D5** (RX) ← Heltec TX, GND ↔ GND.
Configure the Heltec's Meshtastic serial module in `TEXTMSG` mode at 115200 and
**name the node after the firmware `NODE_ID`** (`meshtastic --set-owner "RX01"
--set-owner-short "RX01"`). The full Meshtastic setup is
[Step 5 on `main`](https://github.com/tsuinami-r1/drone-mesh-5plus/blob/main/README.md#step-5--wire-the-node-to-its-heltec-and-configure-meshtastic).

### Step 5 — Give the station a position in the mapper

A Level 1 station has no GPS of its own. `mesh-mapper.py` learns where it is from
the Meshtastic HTTP API (`POST /api/meshtastic_url`) or manual coordinates
(`POST /api/node_location`); see
[Integration with mesh-mapper.py](#integration-with-mesh-mapperpy).

---

## 📡 **RX5808 5.8GHz Analog FM Detection** (`rx5808-detection/`)

Detects **analog FPV video transmitters** operating in the 5.645–5.945 GHz band.
Where the Level 2 firmware looks for digital Remote ID broadcasts, this module uses
the RX5808 analog FM receiver IC to sweep all 40 standard FPV channels and report a
signal hit whenever RSSI exceeds your calibrated threshold.

> **Use case:** Spot FPV racing drones or surveillance UAVs that are broadcasting
> analog video but may *not* carry a Remote ID transmitter.

### Required Hardware

| Component | Notes |
|-----------|-------|
| **Seeed XIAO ESP32-S3** or **XIAO ESP32-C5** | Choose one; the firmware picks the pinout at compile time |
| **RX5808 module** | 5.8GHz analog FM receiver; ~$5–10, widely available |
| Jumper wires | 4 signal + 2 power wires |

### Wiring

Use the **D-pin labels** silkscreened on the board — the GPIO numbers differ between variants but the physical connections are identical.

| RX5808 Pin | Board pin | XIAO ESP32-S3 GPIO | XIAO ESP32-C5 GPIO |
|------------|-----------|-------------------|-------------------|
| DATA | D10 | GPIO9 | GPIO10 |
| CLK | D8 | GPIO7 | GPIO8 |
| CS (active LOW) | D9 | GPIO8 | GPIO9 |
| RSSI (analog in) | D0 | GPIO1 | GPIO2 |
| VCC | 3.3V | — | — |
| GND | GND | — | — |

Optional **Heltec LoRa V3** mesh relay (same physical pins, different GPIOs):

| Signal | Board pin | XIAO ESP32-S3 GPIO | XIAO ESP32-C5 GPIO |
|--------|-----------|-------------------|-------------------|
| ESP32 TX → Heltec RX | D4 | GPIO5 | GPIO6 |
| ESP32 RX ← Heltec TX | D5 | GPIO6 | GPIO7 |

The firmware selects GPIO numbers automatically at compile time based on the target board — no source edits needed.

### Full Flashing Steps

**Step 1 — Set a unique Node ID before flashing (required for multi-node)**

Open `rx5808-detection/src/main.cpp` and change `NODE_ID` to something unique per device. This must match the Meshtastic node's `shortName`/`longName` for range rings to work.

```cpp
// Line ~47 in main.cpp — change for each chip you flash
#define NODE_ID  "RX01"    // e.g. "RX02", "RX03", …
```

**Step 2 — (Optional) Adjust RSSI threshold**

Open `rx5808-detection/src/rx5808.h`. The default of `600` is a safe starting point; tune after calibration. (The RX5808 RSSI output swings ~0–1 V, which is ~0–1320 ADC counts at 12-bit / ADC_11db — so the threshold must sit inside that range.)

```cpp
#define RSSI_THRESHOLD   600   // raise if you get false positives
```

**Step 3 — Wire the hardware**

See the wiring table above. Connect the XIAO to your computer via USB-C.

**Step 4 — Flash**

In VS Code: PlatformIO → Project Tasks → `seeed_xiao_esp32s3` or `seeed_xiao_esp32c5` → **Upload**. Or from the CLI:

```bash
cd rx5808-detection

# XIAO ESP32-S3
pio run -e seeed_xiao_esp32s3 --target upload

# XIAO ESP32-C5
pio run -e seeed_xiao_esp32c5 --target upload
```

> **ESP32-C5 boot mode:** If upload fails with a connection error, hold **BOOT**, tap **RESET**, release **BOOT**, then immediately re-run the command. This forces the chip into download mode. The S3 does not require this.

> **No toolchain at all?** `firmware/` holds prebuilt default-configuration binaries
> (`NODE_ID` = `RX01`) and `esptool` flash commands; see
> [`firmware/README.md`](firmware/README.md).

**Step 5 — Verify on serial monitor**

```bash
pio device monitor --baud 115200
```

You should see within a few seconds:
```json
{"info":"rx5808 scanner ready","node_id":"RX01","receiver":"rx5808","channels":40,"threshold":600,"peak_pick":1}
```

If nothing appears for >5 s, check USB-CDC enumeration: the XIAO waits up to 3 s for a host connection before emitting. Replug and reopen the monitor.

**Step 6 — Calibrate (first-time only)**

*Threshold.* With no FPV transmitter powered:
1. Watch the monitor for 30 seconds. Any `rssi_raw` values that appear are your **noise floor**.
2. Set `RSSI_THRESHOLD` to (noise floor + 200) in `rx5808.h`. Higher values reduce false positives at the cost of missing weaker signals.
3. Reflash (Step 4). Power a known FPV transmitter nearby and confirm detections.

*dBm curve (needed for multi-station fixes).* The station converts the calibrated
ADC millivolt reading to `rssi_dbm` with a two-point line. The defaults are an
unmeasured approximation of a typical RX5808; two stations with different
curves will disagree, and the mapper's position solver only sees their
disagreement as noise. To calibrate: put a VTX on a known channel through a step
attenuator (or at two known distances in the open), note the `rssi_mv` the
station prints at a weak level and at a strong level ~50 dB apart, enter those
(mV, dBm) pairs as `RSSI_CAL_MV_LO/DBM_LO` and `RSSI_CAL_MV_HI/DBM_HI`, and
reflash. Use `RSSI_CAL_OFFSET_DB` for a per-station antenna/cable trim so the
fleet agrees. The heartbeat carries the resulting `threshold_dbm`, which is what
the mapper uses when this station is silent about an emitter.

**Step 7 — Connect to mesh-mapper**

1. Start `mesh-mapper.py` (from `main`) on your collection point.
2. Detections arrive over the mesh via the home node, or for bench testing plug the XIAO straight into the host over USB and select its port in the mapper Settings panel.
3. For range-ring display: set the node's Meshtastic URL or manual coordinates (see Integration section below).

### Configuration

All tuneable constants are at the top of the relevant source files:

| Constant | File | Default | Description |
|----------|------|---------|-------------|
| `RSSI_THRESHOLD` | `src/rx5808.h` | `600` | Raw ADC count above which a channel is reported (RX5808 RSSI spans ~0–1320 counts). Raw counts differ between an S3 and a C5; the threshold only gates reporting, the dBm value is what the mapper compares |
| `RSSI_SAMPLES` | `src/rx5808.h` | `10` | ADC reads per dwell read (raw count + calibrated millivolts each) |
| `TUNE_SETTLE_MS` | `src/rx5808.h` | `30` | ms to wait for RX5808 PLL after tuning |
| `RSSI_CAL_MV_LO` / `RSSI_CAL_DBM_LO` | `src/rx5808.h` | `450` / `-95.0` | Lower calibration point of the mV → dBm line (see [Calibrate](#step-6--calibrate-first-time-only)) |
| `RSSI_CAL_MV_HI` / `RSSI_CAL_DBM_HI` | `src/rx5808.h` | `1100` / `-20.0` | Upper calibration point |
| `RSSI_CAL_OFFSET_DB` | `src/rx5808.h` | `0.0` | Per-station trim (antenna/cable gain) added to every dBm value |
| `NODE_ID` | `src/main.cpp` | `"RX01"` | Change per device; must equal the Meshtastic node name |
| `ENABLE_MESH_RELAY` | `src/main.cpp` | `1` | Set `0` to disable Heltec UART relay |
| `MIN_DWELL_HITS` | `src/main.cpp` | `2` | Every dwell read in a channel visit must clear the threshold |
| `REPORT_INTERVAL_MS` | `src/main.cpp` | `5000` | Minimum ms between USB re-reports of the same channel |
| `MESH_REPORT_INTERVAL_MS` | `src/main.cpp` | `10000` | Minimum ms between mesh re-reports of the same channel (LoRa airtime) |
| `HEARTBEAT_INTERVAL_MS` / `MESH_HEARTBEAT_INTERVAL_MS` | `src/main.cpp` | `60000` / `120000` | Heartbeat cadence on USB / mesh |
| `PEAK_PICK` / `PEAK_WINDOW_MHZ` | `src/main.cpp` | `1` / `20` | Report only the strongest channel of each cluster of adjacent hits (one VTX = one key per station). `0` restores one report per channel |

### Channel Map

All 40 channels scanned per cycle:

| Band | CH1 | CH2 | CH3 | CH4 | CH5 | CH6 | CH7 | CH8 |
|------|-----|-----|-----|-----|-----|-----|-----|-----|
| **R (Raceband)** | 5658 | 5695 | 5732 | 5769 | 5806 | 5843 | 5880 | 5917 |
| **A** | 5865 | 5845 | 5825 | 5805 | 5785 | 5765 | 5745 | 5725 |
| **B** | 5733 | 5752 | 5771 | 5790 | 5809 | 5828 | 5847 | 5866 |
| **E** | 5705 | 5685 | 5665 | 5645 | 5885 | 5905 | 5925 | 5945 |
| **F (Fatshark)** | 5740 | 5760 | 5780 | 5800 | 5820 | 5840 | 5860 | 5880 |

---

## 🔌 **Mapper contract: what a Level 1 station emits**

This is the interface between the branches. `mesh-mapper.py` on `main` consumes
exactly these lines, whether they arrive over USB or through the home node from the
mesh. **Keep this section and the mapper in step.**

### Detection line (USB)

One JSON object per line on USB Serial (115200), whenever a channel clears the
threshold, is the strongest of its cluster of adjacent channels (`PEAK_PICK`),
and the per-channel `REPORT_INTERVAL_MS` has elapsed:

```json
{
  "type":     "analog_fm",
  "receiver": "rx5808",
  "mac":      "AF:00:16:1A:52:01",
  "freq_mhz": 5658,
  "band":     "R",
  "ch":       1,
  "rssi_raw": 850,
  "rssi":     850,
  "rssi_mv":  643,
  "rssi_dbm": -72.7,
  "rssi_min": 812,
  "rssi_max": 891,
  "rssi_n":   20,
  "basic_id": "5.8G-R1-5658MHz",
  "node_id":  "RX01",
  "seq":      42
}
```

| Key | Required | Mapper use |
|-----|----------|------------|
| `type` | yes, must be `"analog_fm"` | Routes the line around every Remote ID code path: no FAA lookup, no drone/pilot markers, no history deque, 30 s stale timeout, `a-u-G-E-S` + range-ring CoT events |
| `mac` | yes | Tracking key. Synthetic, locally-administered `AF:00:` prefix + frequency (big-endian MHz) + band ASCII + channel, so every channel is a distinct "device" and never collides with a real Wi-Fi MAC |
| `node_id` | yes | Looks up the station position (`NODE_LOCATIONS`) and draws the ring there. Must equal the Meshtastic node name |
| `freq_mhz`, `band`, `ch` | yes | Popup, log line, CoT callsign, frequency-derived path loss, emitter clustering (reports within 20 MHz are one emitter) |
| `rssi_raw` | yes | Raw ADC count. Fallback for `rssi_dbm` on the mapper (RX5808 curve) and ring colour when no dBm is present |
| `rssi` | on USB (mapper backfills from `rssi_raw`) | Generic RSSI display shared with Level 2 detections |
| `basic_id` | on USB (mapper backfills from band/ch/freq) | Human-readable label in the detection list |
| `receiver` | no (defaults to `rx5808`) | Which driver produced the line; log tag and popup |
| `rssi_dbm` | no, but needed for fusion | Calibrated received power. Drives the single-station ring radius (`_max_range_m`, FSPL at `freq_mhz`), ring colour (green ≥ −60, amber ≥ −75, red below) and the **multi-station position solver** |
| `rssi_mv` | no | Calibrated ADC millivolts; the number you read off during dBm calibration |
| `rssi_min`, `rssi_max`, `rssi_n` | no | Sample spread behind the report; the solver down-weights a noisy report |
| `seq` | no | Per-station report counter, for spotting mesh loss |

### Detection line (mesh relay, Heltec UART)

When `ENABLE_MESH_RELAY` is on, the same hit goes to the Heltec as one compact
JSON line (`\n`-terminated, no CR, ≤ 190 bytes) at most every
`MESH_REPORT_INTERVAL_MS`. The home node forwards JSON lines to the mapper
unchanged (Level 1 lines bypass its MAC dedup) and tags non-JSON lines `[MESH]`,
which the mapper drops, so the relay line **must** be JSON:

```json
{"type":"analog_fm","mac":"AF:00:16:1A:52:01","freq_mhz":5658,"band":"R","ch":1,"rssi_raw":850,"rssi_dbm":-72.7,"rssi_min":812,"rssi_max":891,"node_id":"RX01","seq":42}
```

The mapper backfills `rssi`, `basic_id` and `receiver` for these.

### Heartbeat and info lines

Every 60 s on USB and every 120 s on the mesh, plus once at boot, the station
emits a status line. The mapper drops any line that carries `heartbeat`,
`status` or `info` and none of the detection keys, so these never create a
phantom device, but it first records the station as **alive** with its
`threshold_dbm`: an alive station that does not report an emitter tells the
position solver the emitter is not within that station's range.

```json
{"heartbeat":true,"node_id":"RX01","receiver":"rx5808","scanning":true,"channels":40,"threshold":600,"threshold_dbm":-94.5,"temp_c":41.2,"uptime_s":3600,"seq":42}
{"info":"rx5808 scanner ready","node_id":"RX01","receiver":"rx5808","channels":40,"threshold":600,"peak_pick":1}
```

### What the mapper does with several stations

Each station on its own still gets a range ring. When two or more positioned
stations report the same emitter (same or adjacent frequency) within the fusion
window, the mapper solves for the emitter position **and** its unknown
transmitter power from the differences between the stations' `rssi_dbm`
(log-distance model, grid search), adds the "not within range of here" constraint
from every alive-but-silent station, and publishes a 🎯 fix with a 90 %
confidence circle at `/api/analog_fixes`. Fixes are also sent to TAK. Tuning is
live at `/api/analog_fusion`. Park stations densely: the fix accuracy is a
fraction of the station spacing.

### Integration with mesh-mapper.py

**What appears in the UI:**

| Without node position | With node position |
|---|---|
| Detection listed in no-GPS panel | Dashed-circle range ring on the map |
| Band, channel, frequency, RSSI shown | 📡 marker at the node's GPS location |
| No map marker | Ring radius = FSPL-derived max detection range |

To get the range ring, give mesh-mapper a position for the station. Two options:

**Option A — Meshtastic HTTP API (automatic, updates every 30 s):**
```
POST /api/meshtastic_url  { "node_id": "RX01", "url": "http://192.168.1.x" }
```
The node's `shortName` or `longName` in Meshtastic must match `NODE_ID` in the firmware (e.g. `"RX01"`). The poller will pick up the correct node even in a multi-node mesh.

**Option B — manual coordinates:**
```
POST /api/node_location  { "node_id": "RX01", "lat": 25.7617, "lon": -80.1918 }
```

Differences from Level 2 (Remote ID) detections in the mapper:
- `type: "analog_fm"` is logged at INFO level with band/channel/RSSI/dBm and skips the FAA lookup.
- Range rings are colored by received power: green ≥ −60 dBm, amber ≥ −75 dBm, red below (raw-count thresholds 1000/800 only when no dBm is available).
- Each detection is also forwarded to ATAK/WinTAK as two CoT events: an `a-u-G-E-S` sensor marker and a `u-r-b-c-c` range ring shape; multi-station fixes add an `a-u-A-M-F-U-M` marker plus a confidence circle.
- A station silent for 30 s is marked inactive (Level 2 detections get 3 min).

---

## 🧭 **Station v2: direction finding + video fingerprint**

The next Level 1 station hardware keeps the RX5808 and adds four sector patch
antennas behind an RF switch (amplitude-comparison bearing, ~±15°) and a video
sync separator on the RX5808's unused video output (confirms a real analog video
carrier, PAL/NTSC fingerprint, kills Wi-Fi false positives). Parts list, pin
budget, power budget, wiring and block diagram are in
[`docs/LEVEL1-V2-HARDWARE.md`](docs/LEVEL1-V2-HARDWARE.md).

## 🗺️ **RX3364 3.3 GHz roadmap**

The next Level 1 receiver is the **RX3364** (3060–3500 MHz analog FM/PLL, 16-channel
FPV band plan). The integration plan, including the shared receiver driver interface
the RX5808 code will be refactored onto, the contract additions the mapper needs, and
the bench characterisation gates, is in
[`docs/RX3364-INTEGRATION-PLAN.md`](docs/RX3364-INTEGRATION-PLAN.md).

---

## 🐛 **Troubleshooting**

**Build fails or the C5 env is missing**
- Make sure the *pioarduino IDE* extension (or a recent PlatformIO CLI) is in use and
  `rx5808-detection/`, not the repo root, is open. The platform is pinned by URL in
  `platformio.ini`; delete the project's `.pio/` folder to force a clean re-download.

**Upload cannot connect**
- On the ESP32-C5: hold **BOOT**, tap **RESET**, release **BOOT**, retry.
- On Linux check the udev rules and `dialout` membership, then
  `ls /dev/ttyACM* /dev/ttyUSB*`.
- With several boards attached set `upload_port` in `platformio.ini`.

**Every channel reports, or nothing ever reports**
- Re-run the threshold calibration. The RSSI line must be on **D0** and the ADC range
  is 0–1320 counts, so a threshold above ~1300 can never trigger and one below the
  noise floor triggers on everything.
- Confirm the module is actually tuning: with a known VTX on, only its channel (and
  the adjacent ones at lower RSSI) should report. If all 40 report identically the
  SPI lines (D10/D8/D9) are miswired.

**Detections reach the mapper but the range ring is missing or misplaced**
- The Meshtastic node's shortName/longName must equal the firmware `NODE_ID`; otherwise
  the mapper falls back to the first GPS node in the mesh. Or set coordinates by hand via
  `/api/node_location`.

**Blank or garbage messages on the mesh at boot**
- The firmware parks the UART TX line high before the USB wait for exactly this
  reason; if it still happens, check that D4 goes to the Heltec **RX** pin and that
  GND is shared.

---

## 📄 **License**

This project is licensed under the MIT License, as is the upstream project it is
derived from.

## 🙏 **Acknowledgments**

- **ColonelPanic** and **Luke Switzer** — the original
  [drone-mesh-mapper](https://github.com/colonelpanichacks/drone-mesh-mapper)
- **"Alik",** 93rd OMBr, and **"Ivan",** 427th Rarog, Armed Forces of Ukraine
- **pioarduino** — the maintained Arduino-ESP32 platform for PlatformIO that makes the C5 build possible
- RotorHazard / Chorus for the RTC6715 synthesizer register formula
