# <div align="center">  **Remote Drone Mapper — Level 1 stations** </div>

<div align="center">

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ESP32](https://img.shields.io/badge/ESP32-S3%20%7C%20C5-green.svg)](https://www.espressif.com/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-pioarduino-orange.svg)](https://github.com/pioarduino/platform-espressif32)
[![Branch](https://img.shields.io/badge/branch-level1--station-blue.svg)](https://github.com/tsuinami-r1/drone-mesh-5plus/tree/level1-station)

**Analog FPV video detection stations for the drone-mesh-5plus counter-surveillance network.**

A Level 1 station is a solar-powered box with four sector antennas that listens
for analog FPV video transmitters, reports **how strong, from which bearing, and
whether it is really video** over a Meshtastic mesh, and lets the mapper place
the drone from what several stations heard.

[⚡ Quick Start](#-quick-start) • [🧭 Hardware](#-hardware) • [🔧 Calibration](#-calibration) • [⚙️ Configuration](#️-configuration-reference) • [🔌 Mapper contract](#-mapper-contract) • [🗺️ Roadmap](#️-roadmap) • [🤝 Contributing](#-contributing)

</div>

---

## 🎯 **What a Level 1 station does**

Every two seconds or so the station sweeps all 40 standard 5.8 GHz FPV channels.
At each channel an RF switch presents the four sector patch antennas in turn, so a
hit carries a calibrated received power on every sector. From the strongest
sector and its two neighbours the station computes a **bearing**. The strongest
hits are then fine-tuned to their carrier centre and held while a sync separator
on the receiver's video output counts line and field pulses, which says whether
the carrier is **really analog video**, PAL or NTSC, and yields a **fingerprint**
that stays with one drone across channels and stations.

| Stations hearing the drone | What the mapper can draw |
|---|---|
| **One** | A range ring ("no further than this") and a **bearing line** from the station |
| **Two or more** | A 🎯 **position fix** with a 90 % confidence circle. Bearings intersect with no assumption about transmitter power; received-power differences and every alive-but-silent station narrow it further |

A single station's signal strength alone was never enough: it cannot tell a
25 mW whoop nearby from a 4 W long-range VTX far away. Bearings and multi-station
fusion are what turn Level 1 from "something is out there" into a position.

**Park stations densely.** Fix accuracy is a fraction of the spacing between
stations, so six cheap boxes beat two clever ones.

> **Use case:** spot FPV racing drones and surveillance UAVs that broadcast analog
> video but carry no Remote ID transmitter, so the Level 2 stations never see them.

### Where this sits in the network

| Tier | Listens for | Branch |
|------|-------------|--------|
| **Level 1** *(this branch)* | Analog FPV **video carriers** — 5.8 GHz now, 3.3 GHz planned. Signal strength per sector, bearing, video fingerprint. | **`level1-station`** |
| **Level 2** | Digital **Remote ID / DJI DroneID / MAVLink** over Wi-Fi and BLE. Decodes drone and pilot GPS. | [`main`](https://github.com/tsuinami-r1/drone-mesh-5plus) |
| Collection point | `mesh-mapper.py`, the home node bridge, TAK output, Raspberry Pi installer | [`main`](https://github.com/tsuinami-r1/drone-mesh-5plus) |

Both tiers share the same Heltec/Meshtastic backhaul and the same D4/D5 UART
wiring. They meet only at the mapper, through the [JSON line contract](#-mapper-contract).

---

## 🧭 **Hardware**

One station is:

| Block | Part | Notes |
|---|---|---|
| Antennas | **4 × 5.8 GHz patch, 8 dBi, ~70° beam**, on the four faces of the box | One batch, matched; linear or RHCP |
| RF switch | **SP4T, 0.1–6 GHz, 3.3 V control** — SKY13322-375LF or PE42442 | Common port replaces the RX5808 whip |
| Receiver | **RX5808** 5.8 GHz analog FM, SPI-modded | Unchanged from earlier stations |
| Video check | **LMH1980** sync separator (3.3 V) on the RX5808 video pin | LM1881 works on a breadboard with a divider |
| MCU | **Seeed XIAO ESP32-S3** or **XIAO ESP32-C5** | Firmware picks the pinout at compile time |
| Mesh | **Heltec WiFi LoRa 32 V3** running Meshtastic | Node name = `NODE_ID` |
| Power | 20 W panel, MPPT charger with 5 V out, 4 × 18650 (≈ 50 Wh) | Station load ≈ 1.3 W |

Two documents carry the full design:

- 📄 [**`docs/LEVEL1-V2-HARDWARE.md`**](docs/LEVEL1-V2-HARDWARE.md) — parts list, pin budget, power budget, block diagram, open questions
- 🖨️ [**`docs/Level1-Station-v2-Bench-Guide.pdf`**](docs/Level1-Station-v2-Bench-Guide.pdf) — printable six-page A4 bench guide: wiring list, BOM with tick boxes, a five-stage procedure with blanks for the values you measure

> ⚠️ **Not yet bench validated.** The firmware compiles and the design is costed,
> but no v2 board has been built. Three values must be measured before a board is
> cut: the RSSI millivolt-to-dBm calibration points, the **SP4T control truth
> table**, and the **XIAO ESP32-C5 pin map** (see the note under wiring).

---

## ⚡ **Quick Start**

### What you need

The [bench guide](docs/Level1-Station-v2-Bench-Guide.pdf) has the complete bill
of materials with tick boxes. In short: the parts in the table above, a computer
with VS Code and a USB-C cable, and the [`main`](https://github.com/tsuinami-r1/drone-mesh-5plus)
branch running `mesh-mapper.py` at the collection point.

<br>

### 1 · Install the toolchain and clone

Firmware builds with **PlatformIO in VS Code via the pioarduino IDE extension**.
Follow [Step 1 on `main`](https://github.com/tsuinami-r1/drone-mesh-5plus/blob/main/README.md#step-1--install-the-toolchain-vs-code--pioarduino)
for the extension, udev rules and the CLI alternative, then:

```bash
git clone --branch level1-station https://github.com/tsuinami-r1/drone-mesh-5plus.git level1-station
```

In VS Code use **File → Open Folder…** on `level1-analog-fpv/` — the project
folder, **not** the repository root, or PlatformIO will not find the environments.

<br>

### 2 · Wire it

Use the **D-pin labels silkscreened on the XIAO**. The firmware takes GPIO numbers
from the board's own Arduino variant, so the same source builds for both boards.

| XIAO pin | S3 GPIO | C5 GPIO | Connects to |
|---|---|---|---|
| **D0** | 1 | 1 | RX5808 RSSI (analog) |
| **D1** | 2 | 0 | SP4T control V1 |
| **D2** | 3 | 25 | SP4T control V2 |
| **D3** | 4 | 7 | SP4T control V3 (spare if the switch decodes 2 lines) |
| **D4** | 5 | 23 | Heltec RX (UART TX) |
| **D5** | 6 | 24 | Heltec TX (UART RX) |
| **D6** | 43 | 11 | Sync separator CSYNC |
| **D7** | 44 | 12 | Sync separator VSYNC |
| **D8** | 7 | 8 | RX5808 CLK |
| **D9** | 8 | 9 | RX5808 CS (active LOW) |
| **D10** | 9 | 10 | RX5808 DATA |
| 3V3 | | | RX5808 VCC, switch VDD, sync separator VDD |
| 5V | | | From the charger's 5 V rail (also feeds the Heltec) |
| GND | | | Everything, one star point |

RF: each patch → switch RF1–RF4 through **equal-length coax ≤ 15 cm**; switch
common → RX5808 antenna pad. Video: RX5808 VIDEO → 0.1 µF → sync separator input,
75 Ω terminated. The full picture is sheet 2 of the [bench guide](docs/Level1-Station-v2-Bench-Guide.pdf).

> ⚠️ **ESP32-C5 pin map.** The C5 column above is what the Arduino core's
> `XIAO_ESP32C5` variant defines, and it is what the firmware uses. Earlier
> firmware on this branch assumed D0 = GPIO2, D4 = GPIO6 and D5 = GPIO7, which
> disagrees. The variant's D6/D7 are the C5's UART0 defaults, exactly as the S3's
> are, so the variant is the one trusted here — but confirm with a continuity
> test on the first C5 board.

<br>

### 3 · Configure the station

Everything lives in `level1-analog-fpv/src/config.h`. Three things must be set
per station before flashing:

```cpp
#define NODE_ID              "RX01"   // unique; must equal the Meshtastic node name (≤ 6 chars keeps mesh lines short)
#define STATION_HEADING_DEG  0        // true bearing of the box face carrying patch N — measure at install
#define SECTOR_SWITCH_TABLE  { {0,0,0}, {1,0,0}, {0,1,0}, {1,1,0} }   // V1,V2,V3 per sector: FROM THE SWITCH DATASHEET
```

A wrong `NODE_ID` draws this station's output at another station's position. A
wrong heading rotates every bearing it reports. A wrong switch table swaps
sectors and points bearings at the wrong quadrant.

<br>

### 4 · Flash

```bash
cd level1-analog-fpv
pio run -e seeed_xiao_esp32s3 --target upload     # XIAO ESP32-S3
pio run -e seeed_xiao_esp32c5 --target upload     # XIAO ESP32-C5
```

> **ESP32-C5 will not connect?** Hold **BOOT**, tap **RESET**, release **BOOT**,
> then immediately re-run. The S3 does not need this.
>
> **No toolchain?** [`firmware/`](firmware/README.md) holds prebuilt binaries with
> the default configuration — `RX01`, heading 0, binary switch table — for bench
> testing only.

<br>

### 5 · Wire the Heltec and name the node

Three wires: XIAO **D4** → Heltec RX, XIAO **D5** ← Heltec TX, GND ↔ GND. Set the
Heltec's Meshtastic serial module to `TEXTMSG` at 115200 and **name the node after
`NODE_ID`**:

```bash
meshtastic --set-owner "RX01" --set-owner-short "RX01"
```

Full Meshtastic setup is [Step 5 on `main`](https://github.com/tsuinami-r1/drone-mesh-5plus/blob/main/README.md#step-5--wire-the-node-to-its-heltec-and-configure-meshtastic).

<br>

### 6 · Verify

```bash
pio device monitor --baud 115200
```

Within a few seconds:

```json
{"info":"rx5808 v2 station ready","node_id":"RX01","receiver":"rx5808","hw":"v2","channels":40,"sectors":4,"heading":0,"threshold":600,"peak_pick":1,"video":1,"fine_tune":1}
```

then a heartbeat every minute. Power a VTX nearby and a detection line should
name its channel, four sector powers, a bearing, and `"video":"NTSC"` or `"PAL"`.
Nothing after 5 s? The XIAO waits up to 3 s for a USB host — replug and reopen
the monitor.

<br>

### 7 · Calibrate, then put it on the map

Do the three [calibrations](#-calibration): threshold (two minutes, required to
work at all), dBm curve (required before this station joins others), and the
bearing pattern (required for bearings to mean anything).

Then start `mesh-mapper.py` from `main` and give the station a position, since it
has no GPS of its own:

```bash
# Automatic — polls the Heltec every 30 s
curl -X POST http://localhost:5000/api/meshtastic_url \
     -d '{"node_id":"RX01","url":"http://192.168.1.x"}' -H 'Content-Type: application/json'

# Or by hand
curl -X POST http://localhost:5000/api/node_location \
     -d '{"node_id":"RX01","lat":25.7617,"lon":-80.1918}' -H 'Content-Type: application/json'
```

Detections arrive over the mesh via the home node. For bench testing, plug the
XIAO into the mapper host over USB and pick its port in the Settings panel.

---

## 🔧 **Calibration**

Three calibrations, each one-time per station. The [bench guide](docs/Level1-Station-v2-Bench-Guide.pdf)
has blanks for every value.

### Threshold — required

With no FPV transmitter powered nearby:

1. Watch the serial monitor for 30 s. Any `rssi_raw` values that appear are the **noise floor**.
2. Set `RSSI_THRESHOLD` to **noise floor + 200** and reflash.
3. Power a known VTX and confirm only its channel and its immediate neighbours report.

### dBm curve — required before a station joins a fleet

`rssi_dbm` comes from the calibrated ADC millivolts through a two-point line whose
**defaults are an unmeasured approximation.** Stations with different curves
disagree about the same signal, and the position solver reads that as noise.

1. Put a VTX on a known channel through a step attenuator, or at two known open-field distances.
2. Note `rssi_mv` at a weak and a strong level about 50 dB apart.
3. Enter the two (mV, dBm) pairs as `RSSI_CAL_MV_LO`/`DBM_LO` and `RSSI_CAL_MV_HI`/`DBM_HI`.
4. Use `RSSI_CAL_OFFSET_DB` for the station's antenna and cable gain, so the fleet agrees on one source.
5. Reflash and check the heartbeat's `threshold_dbm` is sane (roughly −90 to −95).

Raw ADC counts are **not** comparable between an S3 and a C5. `rssi_dbm` is what
the mapper compares; `threshold_dbm` is what it uses to reason about a station that
stays silent.

### Bearing pattern — required for bearings

The bearing is `az[strongest] + K × (P[right] − P[left])` in degrees, with `K` in
degrees per dB from the measured patch pattern.

1. Mount all four patches on the box, put a VTX at 30 m on a known bearing with clear line of sight.
2. Rotate the box in 15° steps through a full turn and log the four `sectors` values at each step.
3. Fit `K` from the neighbour difference and set `BEARING_DEG_PER_DB`; the default 3 °/dB suits an 8 dBi, 70° patch.
4. Save the sweep to `docs/` as the station's pattern.
5. At install, measure the true bearing of the N face and set `STATION_HEADING_DEG`.

---

## ⚙️ **Configuration reference**

All in `level1-analog-fpv/src/config.h`.

**Station**

| Constant | Default | What it does |
|---|---|---|
| `NODE_ID` | `"RX01"` | Unique per station; must equal the Meshtastic node name. Keep it ≤ 6 characters so the worst-case mesh line stays under 200 bytes |
| `STATION_HEADING_DEG` | `0` | True bearing of the N face; added to every reported bearing |
| `STATION_HW` | `"v2"` | Reported as `hw` in every line |

**Receiver and threshold**

| Constant | Default | What it does |
|---|---|---|
| `RSSI_THRESHOLD` | `600` | Raw ADC count a channel must clear (RX5808: ~0–1320) |
| `RSSI_SAMPLES` | `10` | ADC reads per dwell read, raw and calibrated millivolts each |
| `MIN_DWELL_HITS` | `2` | Dwell reads on the strongest sector that must all clear the threshold |
| `RSSI_CAL_MV_LO/HI`, `RSSI_CAL_DBM_LO/HI` | `450/1100`, `−95/−20` | The mV → dBm line |
| `RSSI_CAL_OFFSET_DB` | `0.0` | Per-station trim |

**Sectors and bearing**

| Constant | Default | What it does |
|---|---|---|
| `SECTOR_AZIMUTHS` | `{0, 90, 180, 270}` | Boresight of each patch relative to the N face |
| `SECTOR_SWITCH_TABLE` | 2-bit binary, V3 unused | V1/V2/V3 levels per sector — **from the switch datasheet** |
| `SECTOR_SETTLE_US` | `200` | Wait after a switch change before reading RSSI |
| `BEARING_DEG_PER_DB` | `3.0` | Pattern slope; calibrate |
| `BEARING_MAX_OFFSET_DEG` | `45` | Clamp on the offset inside a quadrant |
| `BEARING_SIGMA_BASE_DEG` | `15` | Reported 1σ for a clean measurement; widened for weak peaks, noise-floor neighbours, or a rear sector nearly as strong (multipath) |

**Video and fingerprint**

| Constant | Default | What it does |
|---|---|---|
| `VIDEO_ENABLE` | `1` | Count sync pulses on the strongest peaks |
| `VIDEO_MEASURE_MS` / `VIDEO_SLICE_MS` | `200` / `10` | Measurement window and the slice size used for `sync_q` |
| `VIDEO_MAX_PER_SWEEP` | `2` | Peaks that get fine-tune + video per sweep (bounds sweep time) |
| `VIDEO_LINE_HZ_MIN/MAX` | `14500` / `17000` | Composite-sync rate that counts as "video-shaped" (wide enough for an LM1881's equalising pulses) |
| `VIDEO_FIELD_TOL_HZ` | `3` | 50 ± 3 → PAL, 60 ± 3 → NTSC |
| `FINE_TUNE_ENABLE`, `FINE_TUNE_SPAN_MHZ`, `FINE_TUNE_STEP_MHZ` | `1`, `10`, `2` | Locate the carrier centre for the fingerprint |

**Reporting**

| Constant | Default | What it does |
|---|---|---|
| `ENABLE_MESH_RELAY` | `1` | `0` disables the Heltec UART relay |
| `REPORT_INTERVAL_MS` / `MESH_REPORT_INTERVAL_MS` | `5000` / `10000` | Minimum ms between re-reports of one channel on USB / mesh |
| `HEARTBEAT_INTERVAL_MS` / `MESH_HEARTBEAT_INTERVAL_MS` | `60000` / `120000` | Heartbeat cadence |
| `PEAK_PICK` / `PEAK_WINDOW_MHZ` | `1` / `20` | Report only the strongest channel of each cluster of adjacent hits |
| `MESH_LINE_MAX` | `200` | Hard limit on a mesh line; optional fields are dropped from the tail rather than truncating JSON |

<details>
<summary><b>Firmware layout</b></summary>

<br>

```
level1-analog-fpv/
  platformio.ini            two envs; -DRECEIVER_RX5808 selects the driver
  src/
    config.h                every pin and tuneable
    main.cpp                sweep, peak-pick, fine-tune + video on the strongest peaks, JSON, relay, heartbeat
    analog_receiver.h       the RX_* interface a receiver driver implements
    receivers/rx5808.*      RTC6715 tuning, RSSI raw + calibrated mV, dBm line
    sector_switch.*         SP4T control lines and sector azimuths
    bearing.*               amplitude-comparison bearing + sigma
    video_sync.*            CSYNC/VSYNC edge counting, PAL/NTSC/none, quality
```

Per sweep: every channel × every sector (≈ 1.8 s), then the strongest due peaks
get a ±10 MHz fine-tune (≈ 0.36 s) and a 200 ms sync count each.

</details>

<details>
<summary><b>Channel map — all 40 channels scanned per sweep</b></summary>

<br>

| Band | CH1 | CH2 | CH3 | CH4 | CH5 | CH6 | CH7 | CH8 |
|------|-----|-----|-----|-----|-----|-----|-----|-----|
| **R (Raceband)** | 5658 | 5695 | 5732 | 5769 | 5806 | 5843 | 5880 | 5917 |
| **A** | 5865 | 5845 | 5825 | 5805 | 5785 | 5765 | 5745 | 5725 |
| **B** | 5733 | 5752 | 5771 | 5790 | 5809 | 5828 | 5847 | 5866 |
| **E** | 5705 | 5685 | 5665 | 5645 | 5885 | 5905 | 5925 | 5945 |
| **F (Fatshark)** | 5740 | 5760 | 5780 | 5800 | 5820 | 5840 | 5860 | 5880 |

Raceband R7 and Band F8 are both 5880 MHz — the same physical frequency.

</details>

---

## 🔌 **Mapper contract**

The interface between the branches: `mesh-mapper.py` on `main` consumes exactly
these lines, over USB or through the home node from the mesh.

> ⚠️ **Rule for contributors:** a change to any emitted key lands together with
> the matching `mesh-mapper.py` change on `main`, and the commit message names
> the `main` commit. Stations in the field are not reflashed when the mapper
> updates, so the mapper keeps accepting older keys.
>
> **Status:** the mapper on `main` consumes everything through `seq` below and
> ignores the v2 keys (`sectors` … `fp`) safely. Teaching the solver to use
> bearings and the fingerprint is the next `main` change — see [Roadmap](#️-roadmap).

### Detection line (USB)

Emitted whenever a channel clears the threshold, is the strongest of its cluster
of adjacent channels, and `REPORT_INTERVAL_MS` has elapsed:

```json
{
  "type":     "analog_fm",
  "receiver": "rx5808",
  "hw":       "v2",
  "mac":      "AF:00:16:6C:52:03",
  "freq_mhz": 5732,
  "band":     "R",
  "ch":       3,
  "rssi_raw": 903,
  "rssi":     903,
  "rssi_mv":  683,
  "rssi_dbm": -68.1,
  "rssi_min": 880,
  "rssi_max": 930,
  "rssi_n":   20,
  "sectors":  [-68.1, -74.4, -91.0, -85.2],
  "sector":   0,
  "bearing_deg": 32,
  "bearing_sigma_deg": 15,
  "freq_peak": 5734,
  "video":    "NTSC",
  "sync_hz":  15736,
  "field_hz": 60,
  "sync_q":   95,
  "fp":       "NTSC/15736/5734",
  "basic_id": "5.8G-R3-5732MHz",
  "node_id":  "RX01",
  "seq":      42
}
```

| Key | Present | What the mapper does with it |
|-----|---------|------------------------------|
| `type` | **always**, literal `"analog_fm"` | Routes the line around every Remote ID path: no FAA lookup, no drone/pilot markers, 30 s stale timeout, sensor + range-ring CoT events |
| `mac` | **always** | Tracking key: `AF:00:` + frequency (big-endian MHz) + band ASCII + channel |
| `node_id` | **always** | Finds the station position; must equal the Meshtastic node name |
| `freq_mhz`, `band`, `ch` | **always** | Popup, log, CoT callsign, frequency-derived path loss, emitter clustering within 20 MHz |
| `rssi_raw`, `rssi` | USB | Raw ADC count; mapper fallback only |
| `rssi_dbm` | **always** | Calibrated power on the strongest sector. Ring radius, ring colour (green ≥ −60, amber ≥ −75), and the multi-station solver |
| `rssi_mv`, `rssi_min`, `rssi_max`, `rssi_n` | USB | Calibration reading and sample spread |
| `receiver`, `hw` | USB | Driver and hardware revision |
| `sectors`, `sector` | USB | dBm on each of the four sectors, and which was strongest |
| `bearing_deg`, `bearing_sigma_deg` | **always** | Bearing from true north and its 1σ. *Mapper: next change* |
| `freq_peak` | when measured | Carrier centre from the fine-tune sweep |
| `video`, `sync_hz`, `field_hz`, `sync_q` | when measured | `NTSC`, `PAL` or `none`; measured line and field rates; quality 0–100 |
| `fp` | when video present | `<standard>/<line_hz>/<freq_peak>` — the per-drone fingerprint. *Mapper: next change* |
| `basic_id` | USB *(backfilled)* | Human-readable label |
| `seq` | **always** | 16-bit report counter, for spotting mesh loss |

### Detection line (mesh relay)

The same hit goes to the Heltec as one compact JSON line, `\n`-terminated with no
CR, at most every `MESH_REPORT_INTERVAL_MS`:

```json
{"type":"analog_fm","mac":"AF:00:16:6C:52:03","freq_mhz":5732,"band":"R","ch":3,"rssi_dbm":-68.1,"bearing_deg":32,"bearing_sigma_deg":15,"fp":"NTSC/15736/5734","node_id":"RX01","seq":42}
```

It **must be JSON** — the home node forwards JSON unchanged (Level 1 lines bypass
its MAC dedup) and drops anything else — and it **must stay under 200 bytes**. The
firmware guarantees that by dropping optional groups from the tail if needed:
first the fingerprint, then the bearing. `"video":"none"` replaces `fp` when video
was measured and absent. The mapper backfills `rssi`, `basic_id` and `receiver`.
Worst case with a 4-character `NODE_ID` is 191 bytes; every extra character of
`NODE_ID` costs one.

### Heartbeat and info lines

Every 60 s on USB and every 120 s on the mesh, plus once at boot. The mapper drops
them as non-detections **after** recording the station as alive with its
`threshold_dbm` — an alive station that does not report an emitter tells the
solver the emitter is not within its range.

```json
{"heartbeat":true,"node_id":"RX01","receiver":"rx5808","hw":"v2","scanning":true,"channels":40,"sectors":4,"heading":0,"threshold":600,"threshold_dbm":-94.5,"video_seen":7,"temp_c":41.2,"uptime_s":3600,"seq":42}
```

The mesh copy carries `node_id`, `receiver`, `hw`, `heading`, `threshold_dbm`,
`video_seen`, `temp_c`, `uptime_s`, `seq`.

### What the mapper does with several stations

When two or more positioned stations report the same emitter (clustered by
frequency within 20 MHz) inside the fusion window, the mapper solves for the
emitter position **and** its unknown transmitter power from the differences in
`rssi_dbm`, adds the "not within range of here" constraint from every
alive-but-silent station, and publishes a fix with a 90 % confidence circle.

| Endpoint on `main` | Gives you |
|---|---|
| `GET /api/analog_fixes` | Current fixes: position, error radius, quality, contributing stations, estimated TX power |
| `GET /api/analog_nodes` | Every Level 1 station heard: alive, threshold, temperature, position |
| `GET`/`POST` `/api/analog_fusion` | Read or tune the solver live |

Fixes go to ATAK/WinTAK as CoT. `mapper_test/analog_fusion_test.py` on `main`
exercises the path offline.

---

## 🗺️ **Roadmap**

| Next | What it adds | Status |
|---|---|---|
| **Bench validation of v2** | Switch truth table, sync separator on a real VTX, RSSI and bearing calibration, C5 pin map | 📋 Procedure in the [bench guide](docs/Level1-Station-v2-Bench-Guide.pdf) |
| **Mapper consumes v2 keys** (`main`) | Bearing residuals in the solver so two stations triangulate with no power assumption; `fp` as a second clustering key; bearing lines and video badges in the UI | 📋 Next `main` change |
| **[RX3364 3.3 GHz receiver](docs/RX3364-INTEGRATION-PLAN.md)** | Long-range analog FPV has moved much traffic to 3.3 GHz. Drops in behind `analog_receiver.h` | 📋 Blocked on gate 0 bench characterisation |

---

## 🤝 **Contributing**

Start here, then read [`CLAUDE.md`](CLAUDE.md) — short, and it holds the rules
that are easy to break by accident.

**The three that matter most:**

1. **This branch is firmware only.** `mesh-mapper.py`, the Level 2 firmware and the home node live on `main` and are never copied here.
2. **Both boards, every time.** Every change must build and behave on the **XIAO ESP32-S3 and the XIAO ESP32-C5**. Pins come from the board variant via `config.h`; never hard-code a GPIO number.
3. **Changing an emitted JSON key changes the contract.** The matching `mesh-mapper.py` change lands on `main` in the same change set, and the commit message names the `main` commit.

**Before you commit:**

```bash
cd level1-analog-fpv
pio run -e seeed_xiao_esp32s3 -e seeed_xiao_esp32c5     # both must succeed, no warnings in src/
```

If the emitted JSON changed, rebuild the prebuilt binaries in `firmware/` from the
default configuration and run `mapper_test/analog_fusion_test.py` on `main`.

**Traps worth knowing about:**

- The RX5808 synthesizer register B is a **split N/A field**, `((tf/32)<<7) | (tf%32)`. The flat `(freq-479)/2` form mistunes by about 4 GHz and detects nothing.
- ADC attenuation is **per-pin** (`analogSetPinAttenuation`), not global.
- Mesh lines are `\n`-terminated with **no CR**, written as one burst, and never longer than `MESH_LINE_MAX`. Meshtastic's TEXTMSG serial module broadcasts raw chunks.
- `RX_CHANNEL_COUNT` and `SECTOR_COUNT` are `#define`s so they can size stack arrays; `static_assert`s keep them honest.
- The video classifier decides PAL/NTSC by **field** rate. An LM1881's composite sync carries equalising pulses that lift the line-rate count ~500 Hz; do not tighten `VIDEO_LINE_HZ_MIN/MAX` to the nominal line rates.

---

## 🐛 **Troubleshooting**

<details open>
<summary><b>Every channel reports, or nothing ever reports</b></summary>

- Re-run the threshold calibration. The ADC range is 0–1320 counts, so a threshold above ~1300 can never trigger and one below the noise floor triggers on everything.
- Confirm the RSSI line is on **D0**, and the receiver is actually tuning: with a known VTX on, only its channel and its immediate neighbours should report. If all 40 report identically, the SPI lines (D10/D8/D9) are miswired.

</details>

<details>
<summary><b>All four sectors read the same, or bearings point at the wrong quadrant</b></summary>

- Same on all four: the switch is not switching. Check the control lines on D1/D2/D3 and `SECTOR_SWITCH_TABLE` against the datasheet; on a C5, check the pin map note under wiring.
- Wrong quadrant: `SECTOR_SWITCH_TABLE` order does not match the physical patches, or `STATION_HEADING_DEG` is wrong. `sectors` in the USB line shows which index was strongest; walk a VTX around the box and check the index follows it.

</details>

<details>
<summary><b><code>video</code> is always <code>none</code> on a real VTX</b></summary>

- Scope the sync separator's CSYNC output: expect a 15.6–15.7 kHz train (up to ~16.3 kHz on an LM1881). No train: check the 0.1 µF coupling and 75 Ω termination on the RX5808 VIDEO pin.
- Train present but `none`: check `field_hz` in the USB line. It must read 50 or 60; a VSYNC wiring fault leaves it 0.
- LM1881 outputs are 5 V — the divider to the XIAO is required.

</details>

<details>
<summary><b>Detections reach the mapper but the ring or bearing is in the wrong place</b></summary>

- The Meshtastic node's shortName/longName must equal `NODE_ID`, or the mapper falls back to the first GPS node in the mesh.
- Or set the coordinates by hand with `POST /api/node_location`.

</details>

<details>
<summary><b>Stations disagree about the same drone, or fixes look wrong</b></summary>

- Run the dBm calibration on every station; uncalibrated stations disagree by many dB.
- Check each station's `RSSI_CAL_OFFSET_DB` and `STATION_HEADING_DEG`.
- Check station positions: `GET /api/analog_nodes`.

</details>

<details>
<summary><b>Build fails, or the C5 environment is missing</b></summary>

- Use the *pioarduino IDE* extension (or a recent PlatformIO CLI) with `level1-analog-fpv/` open — not the repository root.
- The platform is pinned by URL in `platformio.ini`; delete `.pio/` to force a clean re-download.
- `#error … supports the Seeed XIAO ESP32-S3 and XIAO ESP32-C5 only`: you selected a different board; the pin map comes from the XIAO variants.

</details>

<details>
<summary><b>Upload cannot connect</b></summary>

- ESP32-C5: hold **BOOT**, tap **RESET**, release **BOOT**, retry.
- On Linux check udev rules and `dialout` membership, then `ls /dev/ttyACM* /dev/ttyUSB*`.
- With several boards attached, set `upload_port` in `platformio.ini`.

</details>

<details>
<summary><b>Blank or garbage messages on the mesh at boot</b></summary>

- The firmware parks the UART TX line high before the USB wait for exactly this reason. If it still happens, check that D4 goes to the Heltec **RX** pin and that GND is shared.

</details>

---

## 📄 **License**

MIT, as is the upstream project it is derived from.

## 🙏 **Acknowledgments**

- **ColonelPanic** and **Luke Switzer** — the original [drone-mesh-mapper](https://github.com/colonelpanichacks/drone-mesh-mapper)
- **"Alik",** 93rd OMBr, and **"Ivan",** 427th Rarog, Armed Forces of Ukraine
- **pioarduino** — the maintained Arduino-ESP32 platform for PlatformIO that makes the C5 build possible
- **RotorHazard / Chorus** — the RTC6715 synthesizer register formula
