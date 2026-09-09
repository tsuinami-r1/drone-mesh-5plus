# <div align="center">  **Remote Drone Mapper — Level 1 stations** </div>

<div align="center">

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![ESP32](https://img.shields.io/badge/ESP32-S3%20%7C%20C5-green.svg)](https://www.espressif.com/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-pioarduino-orange.svg)](https://github.com/pioarduino/platform-espressif32)
[![Branch](https://img.shields.io/badge/branch-level1--station-blue.svg)](https://github.com/tsuinami-r1/drone-mesh-5plus/tree/level1-station)

**Analog FPV video detection stations for the drone-mesh-5plus counter-surveillance network.**

A Level 1 station is a solar-powered box that listens for analog FPV video
transmitters, reports what it hears over a Meshtastic mesh, and lets the mapper
work out **where the drone is** from what several stations heard.

[⚡ Quick Start](#-quick-start) • [🧭 Build a station](#-hardware-revisions) • [🔧 Calibration](#-calibration) • [⚙️ Configuration](#️-configuration-reference) • [🔌 Mapper contract](#-mapper-contract) • [🗺️ Roadmap](#️-roadmap) • [🤝 Contributing](#-contributing)

</div>

---

## 🎯 **What a Level 1 station does**

The RX5808 analog FM receiver sweeps all 40 standard 5.8 GHz FPV channels about
once every two seconds. When a channel's signal clears the station's calibrated
threshold, the station emits one JSON line naming the frequency and the **received
power in dBm**, over USB and over the LoRa mesh.

That single line is not a position — one station cannot know how far away a
transmitter is, because it cannot know how much power the transmitter is using. A
25 mW whoop and a 4 W long-range VTX differ by more than 20 dB. So:

| Stations hearing the drone | What the mapper draws |
|---|---|
| **One** | A range ring around the station: "no further away than this" |
| **Two or more** | A 🎯 **position fix** with a 90 % confidence circle. The unknown transmitter power cancels out in the differences between stations, and every station that is *alive but silent* narrows the answer further |

**This is why stations are parked densely.** Fix accuracy is a fraction of the
spacing between stations, so six cheap boxes beat two clever ones.

> **Use case:** spot FPV racing drones and surveillance UAVs that broadcast analog
> video but carry no Remote ID transmitter, so the Level 2 stations never see them.

### Where this sits in the network

| Tier | Listens for | Branch |
|------|-------------|--------|
| **Level 1** *(this branch)* | Analog FPV **video carriers** — 5.8 GHz now, 3.3 GHz planned. No decoding, signal strength only. | **`level1-station`** |
| **Level 2** | Digital **Remote ID / DJI DroneID / MAVLink** over Wi-Fi and BLE. Decodes drone and pilot GPS. | [`main`](https://github.com/tsuinami-r1/drone-mesh-5plus) |
| Collection point | `mesh-mapper.py`, the home node bridge, TAK output, Raspberry Pi installer | [`main`](https://github.com/tsuinami-r1/drone-mesh-5plus) |

Both tiers share the same Heltec/Meshtastic backhaul and the same D4/D5 UART
wiring, so one carrier PCB serves either. They meet only at the mapper, through
the [JSON line contract](#-mapper-contract).

---

## 🧭 **Hardware revisions**

Two revisions exist. **Build v1 today** — it is what the firmware in this branch
runs. v2 is a design on paper: the hardware is specified and costed, but no board
has been built and no firmware implements it yet.

| | **v1 — fielded** | **v2 — design only** |
|---|---|---|
| Antenna | One 5.8 GHz omni | **Four sector patches + SP4T RF switch** |
| Gives you | Signal strength per channel | Strength **plus a bearing**, ±15° |
| Video pin | Unused | **Sync separator**: confirms a real video carrier, PAL/NTSC fingerprint, rejects Wi-Fi false positives |
| Power | ~1.3 W | ~1.3 W (the switch adds nothing measurable) |
| Cost | ≈ $45 + power | ≈ $190 all-in with panel and battery |
| Status | ✅ Firmware in this branch | 📐 Specified, **not yet bench validated** |

**Building v2?** Everything you need is in two documents:

- 📄 [**`docs/LEVEL1-V2-HARDWARE.md`**](docs/LEVEL1-V2-HARDWARE.md) — parts list, pin budget, power budget, block diagram, firmware plan, open questions
- 🖨️ [**`docs/Level1-Station-v2-Bench-Guide.pdf`**](docs/Level1-Station-v2-Bench-Guide.pdf) — the same thing as a printable six-page A4 bench guide: wiring list, BOM with tick boxes, and a five-stage procedure with blanks for the values you measure

Three values in the v2 design are unverified and must be measured before a board
is cut: the RSSI millivolt-to-dBm points, the XIAO ESP32-C5 D6/D7 GPIO numbers,
and the SP4T control truth table.

---

## ⚡ **Quick Start**

Build a v1 station and get it onto the map. About an hour, most of it waiting for
the toolchain to download.

### What you need

| Role | Part | Qty |
|------|------|-----|
| MCU | Seeed **XIAO ESP32-S3** or **XIAO ESP32-C5** | 1 per station |
| Receiver | **RX5808** 5.8 GHz analog FM module | 1 per station |
| Mesh radio | **Heltec WiFi LoRa 32 V3** running Meshtastic | 1 per station |
| Wiring | 4 signal + 2 power jumpers, plus 3 to the Heltec | — |
| Collection point | The [`main`](https://github.com/tsuinami-r1/drone-mesh-5plus) branch: `mesh-mapper.py` + home node | 1 per network |
| Build machine | A computer with VS Code and a USB-C cable | 1 |

<br>

### 1 · Install the toolchain and clone

Firmware builds with **PlatformIO in VS Code via the pioarduino IDE extension**.
Follow [Step 1 on `main`](https://github.com/tsuinami-r1/drone-mesh-5plus/blob/main/README.md#step-1--install-the-toolchain-vs-code--pioarduino)
for the extension, udev rules and the CLI alternative, then:

```bash
git clone --branch level1-station https://github.com/tsuinami-r1/drone-mesh-5plus.git level1-station
```

In VS Code use **File → Open Folder…** on `rx5808-detection/` — the project
folder, **not** the repository root, or PlatformIO will not find the environments.

<br>

### 2 · Wire the RX5808 to the XIAO

Use the **D-pin labels silkscreened on the board**. GPIO numbers differ between
the S3 and the C5; the firmware picks the right ones at compile time, so the
physical connections are identical either way.

| RX5808 pin | XIAO pin | S3 GPIO | C5 GPIO |
|---|---|---|---|
| DATA | **D10** | 9 | 10 |
| CLK | **D8** | 7 | 8 |
| CS *(active LOW)* | **D9** | 8 | 9 |
| RSSI *(analog)* | **D0** | 1 | 2 |
| VCC | 3V3 | — | — |
| GND | GND | — | — |

> 🧭 Building v2 instead? The four-patch, RF-switch and sync-separator wiring is on
> sheet 2 of the [bench guide PDF](docs/Level1-Station-v2-Bench-Guide.pdf).

<br>

### 3 · Set the station's ID and flash

Open `rx5808-detection/src/main.cpp` and give this station a unique name. **It
must match the Meshtastic node name you set in step 4**, or the mapper will draw
this station's rings in the wrong place.

```cpp
#define NODE_ID  "RX01"    // RX02, RX03, … one per station
```

Then flash:

```bash
cd rx5808-detection
pio run -e seeed_xiao_esp32s3 --target upload     # XIAO ESP32-S3
pio run -e seeed_xiao_esp32c5 --target upload     # XIAO ESP32-C5
```

> **ESP32-C5 will not connect?** Hold **BOOT**, tap **RESET**, release **BOOT**,
> then immediately re-run. The S3 does not need this.
>
> **No toolchain at all?** [`firmware/`](firmware/README.md) holds prebuilt
> binaries and `esptool` commands — but they are all `NODE_ID` = `RX01`, so they
> are for bench testing only, never for a second fielded station.

<br>

### 4 · Wire the Heltec and name the node

Three wires: XIAO **D4** (TX) → Heltec RX, XIAO **D5** (RX) ← Heltec TX, GND ↔ GND.

Set the Heltec's Meshtastic serial module to `TEXTMSG` at 115200, and **name the
node after `NODE_ID`**:

```bash
meshtastic --set-owner "RX01" --set-owner-short "RX01"
```

Full Meshtastic setup is [Step 5 on `main`](https://github.com/tsuinami-r1/drone-mesh-5plus/blob/main/README.md#step-5--wire-the-node-to-its-heltec-and-configure-meshtastic).

<br>

### 5 · Verify on the serial monitor

```bash
pio device monitor --baud 115200
```

Within a few seconds you should see the boot line, then a heartbeat every minute:

```json
{"info":"rx5808 scanner ready","node_id":"RX01","receiver":"rx5808","channels":40,"threshold":600,"peak_pick":1}
```

Nothing after 5 s? The XIAO waits up to 3 s for a USB host before it says
anything — replug and reopen the monitor.

<br>

### 6 · Calibrate, then put it on the map

Do the [threshold calibration](#-calibration) now; it takes two minutes and
nothing works properly without it. Do the dBm calibration before this station
joins others in the field.

Then start `mesh-mapper.py` from `main` and give the station a position, because
a Level 1 station has no GPS of its own:

```bash
# Automatic — polls the Heltec every 30 s
curl -X POST http://localhost:5000/api/meshtastic_url \
     -d '{"node_id":"RX01","url":"http://192.168.1.x"}' -H 'Content-Type: application/json'

# Or set it by hand
curl -X POST http://localhost:5000/api/node_location \
     -d '{"node_id":"RX01","lat":25.7617,"lon":-80.1918}' -H 'Content-Type: application/json'
```

Detections arrive over the mesh via the home node. For bench testing, plug the
XIAO straight into the mapper host over USB and pick its port in the Settings
panel.

**Done.** One station draws a range ring. Add a second and third within a few
hundred metres and the mapper starts producing position fixes.

---

## 🔧 **Calibration**

Two calibrations, both one-time per station. The first makes the station work at
all; the second makes it agree with its neighbours, which is what multi-station
fixes are built on.

### Threshold — required

With no FPV transmitter powered anywhere nearby:

1. Watch the serial monitor for 30 seconds. Any `rssi_raw` values that appear are your **noise floor**.
2. Set `RSSI_THRESHOLD` in `src/rx5808.h` to **noise floor + 200**.
3. Reflash. Power a known VTX nearby and confirm only its channel (and its immediate neighbours, weaker) report.

### dBm curve — required before a station joins a fleet

The station converts its calibrated ADC millivolt reading into `rssi_dbm` with a
two-point straight line. **The defaults are an unmeasured approximation.** Two
stations with different curves disagree about the same signal, and the mapper's
position solver can only read that disagreement as noise.

1. Put a VTX on a known channel through a step attenuator, or at two known distances in the open.
2. Note the `rssi_mv` the station prints at a weak level and at a strong level, about 50 dB apart.
3. Enter those two (mV, dBm) pairs as `RSSI_CAL_MV_LO`/`RSSI_CAL_DBM_LO` and `RSSI_CAL_MV_HI`/`RSSI_CAL_DBM_HI` in `src/rx5808.h`.
4. Use `RSSI_CAL_OFFSET_DB` as a per-station trim for antenna and cable gain, so the whole fleet agrees on one source.
5. Reflash and confirm the heartbeat reports a sane `threshold_dbm` — expect roughly −90 to −95.

> Raw ADC counts are **not** comparable between an S3 and a C5; their ADCs have
> different transfer curves. `rssi_dbm` is the number the mapper compares, and
> `threshold_dbm` is what it uses to reason about a station that stays silent.

The [bench guide PDF](docs/Level1-Station-v2-Bench-Guide.pdf) has a blank
calibration table on sheet 4 to fill in as you go.

---

## ⚙️ **Configuration reference**

Every tuneable is a `#define` at the top of its source file.

**`src/main.cpp` — station identity and reporting**

| Constant | Default | What it does |
|---|---|---|
| `NODE_ID` | `"RX01"` | Unique per station; **must equal the Meshtastic node name** |
| `ENABLE_MESH_RELAY` | `1` | `0` disables the Heltec UART relay |
| `MIN_DWELL_HITS` | `2` | Every dwell read in a channel visit must clear the threshold to count |
| `REPORT_INTERVAL_MS` | `5000` | Minimum ms between USB re-reports of one channel |
| `MESH_REPORT_INTERVAL_MS` | `10000` | Same for the mesh. LoRa airtime is the scarce resource in a dense deployment |
| `HEARTBEAT_INTERVAL_MS` | `60000` | Heartbeat cadence on USB |
| `MESH_HEARTBEAT_INTERVAL_MS` | `120000` | Heartbeat cadence on the mesh |
| `PEAK_PICK` / `PEAK_WINDOW_MHZ` | `1` / `20` | Report only the strongest channel of each cluster of adjacent hits, so one VTX is one tracking key. `0` restores one report per channel |

**`src/rx5808.h` — receiver and calibration**

| Constant | Default | What it does |
|---|---|---|
| `RSSI_THRESHOLD` | `600` | Raw ADC count above which a channel reports. RX5808 RSSI spans ~0–1320 counts |
| `RSSI_SAMPLES` | `10` | ADC reads per dwell read (raw count and calibrated millivolts each) |
| `TUNE_SETTLE_MS` | `30` | PLL settle after tuning. **Do not go below 25** |
| `RSSI_CAL_MV_LO` / `RSSI_CAL_DBM_LO` | `450` / `-95.0` | Lower point of the mV → dBm line |
| `RSSI_CAL_MV_HI` / `RSSI_CAL_DBM_HI` | `1100` / `-20.0` | Upper point |
| `RSSI_CAL_OFFSET_DB` | `0.0` | Per-station antenna/cable trim added to every dBm value |

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

This is the interface between the branches: `mesh-mapper.py` on `main` consumes
exactly these lines, whether they arrive over USB or through the home node from
the mesh.

> ⚠️ **Rule for contributors:** a change to any emitted key must land together
> with the matching `mesh-mapper.py` change on `main`, and the commit message must
> name the `main` commit. Stations already in the field are not reflashed when the
> mapper updates, so the mapper must keep accepting the old keys.

### Detection line (USB)

Emitted whenever a channel clears the threshold, is the strongest of its cluster
of adjacent channels, and `REPORT_INTERVAL_MS` has elapsed:

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

| Key | Required | What the mapper does with it |
|-----|----------|------------|
| `type` | **yes**, literal `"analog_fm"` | Routes the line around every Remote ID path: no FAA lookup, no drone/pilot markers, no history deque, 30 s stale timeout, `a-u-G-E-S` + range-ring CoT events |
| `mac` | **yes** | Tracking key. Synthetic `AF:00:` + frequency (big-endian MHz) + band ASCII + channel, so every channel is its own "device" and never collides with a real Wi-Fi MAC |
| `node_id` | **yes** | Finds the station position and draws the ring there. Must equal the Meshtastic node name |
| `freq_mhz`, `band`, `ch` | **yes** | Popup, log line, CoT callsign, frequency-derived path loss, and emitter clustering — reports within 20 MHz are treated as one emitter |
| `rssi_raw` | **yes** | Raw ADC count. Only a fallback for `rssi_dbm`, and for ring colour when no dBm is present |
| `rssi` | no *(backfilled)* | Generic RSSI display shared with Level 2 detections |
| `basic_id` | no *(backfilled)* | Human-readable label in the detection list |
| `receiver` | no *(defaults `rx5808`)* | Which driver produced the line; log tag and popup |
| `rssi_dbm` | no, **but needed for fixes** | Calibrated received power. Drives the ring radius, the ring colour (green ≥ −60, amber ≥ −75, red below) and the multi-station position solver |
| `rssi_mv` | no | Calibrated millivolts — the number you read off during dBm calibration |
| `rssi_min`, `rssi_max`, `rssi_n` | no | Sample spread; the solver down-weights a noisy report |
| `seq` | no | Per-station report counter, for spotting mesh loss |

### Detection line (mesh relay)

The same hit goes to the Heltec as one compact JSON line, `\n`-terminated with no
CR, at most every `MESH_REPORT_INTERVAL_MS`. It must stay **under ~190 bytes** for
the LoRa payload:

```json
{"type":"analog_fm","mac":"AF:00:16:1A:52:01","freq_mhz":5658,"band":"R","ch":1,"rssi_raw":850,"rssi_dbm":-72.7,"rssi_min":812,"rssi_max":891,"node_id":"RX01","seq":42}
```

It **must be JSON**: the home node forwards JSON to the mapper unchanged (Level 1
lines bypass its MAC dedup, because several stations legitimately report the same
synthetic MAC) and tags anything else `[MESH]`, which the mapper drops. The mapper
backfills `rssi`, `basic_id` and `receiver`.

### Heartbeat and info lines

Every 60 s on USB, every 120 s on the mesh, plus once at boot:

```json
{"heartbeat":true,"node_id":"RX01","receiver":"rx5808","scanning":true,"channels":40,"threshold":600,"threshold_dbm":-94.5,"temp_c":41.2,"uptime_s":3600,"seq":42}
{"info":"rx5808 scanner ready","node_id":"RX01","receiver":"rx5808","channels":40,"threshold":600,"peak_pick":1}
```

The mapper drops any line carrying `heartbeat`, `status` or `info` and none of the
detection keys, so these never create a phantom device — **but it first records
the station as alive**, with its `threshold_dbm`. That matters: an alive station
that does *not* report an emitter is telling the solver the emitter is not within
its range, which is what lets a dense field of stations narrow a fix.

### What the mapper does with several stations

When two or more positioned stations report the same emitter within the fusion
window, the mapper solves for the emitter position **and** its unknown transmitter
power from the differences between their `rssi_dbm` values, adds the "not within
range of here" constraint from every alive-but-silent station, and publishes a fix
with a 90 % confidence circle.

| Endpoint on `main` | Gives you |
|---|---|
| `GET /api/analog_fixes` | Current fixes: position, error radius, quality, contributing stations, estimated TX power |
| `GET /api/analog_nodes` | Every Level 1 station heard: alive, threshold, temperature, position |
| `GET`/`POST` `/api/analog_fusion` | Read or tune the solver live (path-loss exponent, sigma, silent margin, window, clustering) |

Fixes are also pushed to ATAK/WinTAK as CoT. `mapper_test/analog_fusion_test.py`
on `main` exercises the whole path offline, with no hardware.

<details>
<summary><b>What appears in the mapper UI</b></summary>

<br>

| Station has no position | Station has a position | Two or more stations |
|---|---|---|
| Detection listed in the no-GPS panel | Dashed range ring on the map | 🎯 fix marker with a 90 % circle |
| Band, channel, frequency, RSSI shown | 📡 marker at the station | Spokes to each contributing station |
| No map marker | Ring radius from received power | ❔ ghost marker if a mirror solution cannot be ruled out |

Other differences from Level 2 detections: analog lines skip the FAA lookup, and a
station silent for 30 s is marked inactive where Level 2 detections get 3 minutes.

</details>

---

## 🗺️ **Roadmap**

| Next | What it adds | Status |
|---|---|---|
| **[Station v2 hardware](docs/LEVEL1-V2-HARDWARE.md)** | Four sector patches + RF switch for a ±15° bearing; video sync separator for a real-video check and a per-drone fingerprint | 📐 Specified and costed. Needs bench validation, then firmware |
| **[RX3364 3.3 GHz receiver](docs/RX3364-INTEGRATION-PLAN.md)** | Long-range analog FPV has moved a lot of traffic to 3.3 GHz, where a 5.8 GHz-only station sees nothing | 📋 Planned. Blocked on gate 0 bench characterisation |

Both plans reuse the same additive-key approach to the mapper contract, so
stations already in the field keep working while new ones report more.

---

## 🤝 **Contributing**

Start here, then read [`CLAUDE.md`](CLAUDE.md) — it is short and holds the rules
that are easy to break by accident.

**The three that matter most:**

1. **This branch is firmware only.** `mesh-mapper.py`, the Level 2 firmware and the home node live on `main` and are never copied here. There is exactly one mapper to stay compatible with.
2. **Both boards, every time.** Every change must compile and behave correctly for the **XIAO ESP32-S3 and the XIAO ESP32-C5**. Check both sides of every `#if defined(CONFIG_IDF_TARGET_ESP32C5)` conditional and every GPIO number.
3. **Changing an emitted JSON key changes the contract.** The matching `mesh-mapper.py` change lands on `main` in the same change set, and the commit message names the `main` commit.

**Before you commit:**

```bash
cd rx5808-detection
pio run -e seeed_xiao_esp32s3 -e seeed_xiao_esp32c5     # both must succeed
```

If the emitted JSON changed, also rebuild the prebuilt binaries in `firmware/`
from the default configuration, and run `mapper_test/analog_fusion_test.py` on
`main` against your change.

**Traps worth knowing about:**

- The RX5808 synthesizer register B is a **split N/A field**, `((tf/32)<<7) | (tf%32)`. The flat `(freq-479)/2` form mistunes by about 4 GHz and detects nothing.
- ADC attenuation is **per-pin** (`analogSetPinAttenuation`), not global.
- Mesh lines are `\n`-terminated with **no CR**, written as one burst. Meshtastic's TEXTMSG serial module broadcasts raw chunks, so a stray CR rides along inside the mesh message.
- `FPV_CHANNEL_COUNT` is a `#define`, not a `const int`, so it can size stack arrays. A `static_assert` keeps it honest.

---

## 🐛 **Troubleshooting**

<details open>
<summary><b>Every channel reports, or nothing ever reports</b></summary>

- Re-run the threshold calibration. The ADC range is 0–1320 counts, so a threshold above ~1300 can never trigger and one below the noise floor triggers on everything.
- Confirm the RSSI line is on **D0**.
- Confirm the receiver is actually tuning: with a known VTX on, only its channel and its immediate neighbours should report. If all 40 report identically, the SPI lines (D10/D8/D9) are miswired.

</details>

<details>
<summary><b>Detections reach the mapper but the ring is missing or in the wrong place</b></summary>

- The Meshtastic node's shortName/longName must equal the firmware `NODE_ID`. Otherwise the mapper falls back to the first GPS node in the mesh and draws the ring at someone else's position.
- Or set the coordinates by hand with `POST /api/node_location`.

</details>

<details>
<summary><b>Stations disagree about the same drone, or fixes look wrong</b></summary>

- Run the [dBm calibration](#-calibration) on every station. Uncalibrated stations disagree by many dB and the solver reads that as noise.
- Check each station's `RSSI_CAL_OFFSET_DB` accounts for its antenna and cable.
- Confirm each station's position in the mapper is right: `GET /api/analog_nodes`.

</details>

<details>
<summary><b>Build fails, or the C5 environment is missing</b></summary>

- Make sure the *pioarduino IDE* extension (or a recent PlatformIO CLI) is in use, and that `rx5808-detection/` is open — not the repository root.
- The platform is pinned by URL in `platformio.ini`; delete the project's `.pio/` folder to force a clean re-download.

</details>

<details>
<summary><b>Upload cannot connect</b></summary>

- ESP32-C5: hold **BOOT**, tap **RESET**, release **BOOT**, retry.
- On Linux check the udev rules and `dialout` membership, then `ls /dev/ttyACM* /dev/ttyUSB*`.
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
