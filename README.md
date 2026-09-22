# <div align="center">  **Remote Drone Mapper — Level 1 stations** </div>

<div align="center">

[![License: MIT + GPL-3.0](https://img.shields.io/badge/License-MIT%20%2B%20GPL--3.0-yellow.svg)](#-license)
[![ESP32](https://img.shields.io/badge/ESP32-C5%20(v3)%20%7C%20S3%20%2B%20C5%20(v2)-green.svg)](https://www.espressif.com/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-pioarduino-orange.svg)](https://github.com/pioarduino/platform-espressif32)
[![Branch](https://img.shields.io/badge/branch-level1--c5phy-blue.svg)](https://github.com/tsuinami-r1/drone-mesh-5plus/tree/level1-c5phy)

**Analog FPV video detection stations for the drone-mesh-5plus counter-surveillance network.**

A Level 1 station is a solar-powered box with four sector antennas that listens
for analog FPV video transmitters, reports **how strong, from which bearing, and
whether it is really video** over a Meshtastic mesh, and lets the mapper place
the drone from what several stations heard.

**This branch carries the v3 prototype: the XIAO ESP32-C5's own 5 GHz Wi-Fi radio
is the receiver.** No RX5808, no sync separator. The v2 station (RX5808 + sync
separator) it replaces stays on this branch unchanged as the reference design.

[⚡ Quick Start](#-quick-start) • [🧭 Hardware](#-hardware) • [🧪 Bench validation](#-bench-validation-the-gate-before-fielding) • [🔧 Calibration](#-calibration) • [⚙️ Configuration](#️-configuration-reference) • [🔌 Mapper contract](#-mapper-contract) • [🤝 Contributing](#-contributing)

</div>

---

## 🎯 **What a Level 1 station does**

Every two seconds or so the station sweeps all 40 standard 5.8 GHz FPV channels.
At each channel an RF switch presents the four sector patch antennas in turn, so a
hit carries a calibrated received power on every sector. From the strongest
sector and its two neighbours the station computes a **bearing**. The strongest
hits are then held while the carrier is demodulated and checked for a
horizontal-sync train at the PAL or NTSC line period, which says whether the
carrier is **really analog video** and yields a **fingerprint** that stays with
one drone across channels and stations.

| Stations hearing the drone | What the mapper can draw |
|---|---|
| **One** | A range ring ("no further than this") and a **bearing line** from the station |
| **Two or more** | A 🎯 **position fix** with a 90 % confidence circle. Bearings intersect with no assumption about transmitter power; received-power differences and every alive-but-silent station narrow it further |

**Park stations densely.** Fix accuracy is a fraction of the spacing between
stations, so six cheap boxes beat two clever ones.

> **Use case:** spot FPV racing drones and surveillance UAVs that broadcast analog
> video but carry no Remote ID transmitter, so the Level 2 stations never see them.

### Where this sits in the network

| Tier | Listens for | Branch |
|------|-------------|--------|
| **Level 1 v3 prototype** *(this branch)* | Analog FPV **video carriers**, 5.8 GHz, on the **C5's own radio**. Power per sector, bearing, software video check | **`level1-c5phy`** |
| Level 1 v2 | Same job on an **RX5808 + sync separator**; XIAO S3 or C5 | [`level1-station`](https://github.com/tsuinami-r1/drone-mesh-5plus/tree/level1-station) (also kept here in `level1-analog-fpv/`) |
| Level 2 | Digital **Remote ID / DJI DroneID / MAVLink** over Wi-Fi and BLE. Decodes drone and pilot GPS | [`main`](https://github.com/tsuinami-r1/drone-mesh-5plus) |
| Collection point | `mesh-mapper.py`, the home node bridge, TAK output, Raspberry Pi installer | [`main`](https://github.com/tsuinami-r1/drone-mesh-5plus) |

All tiers share the same Heltec/Meshtastic backhaul and the same **D4/D5** UART
wiring. They meet only at the mapper, through the [JSON line contract](#-mapper-contract),
which the v3 station speaks unchanged.

---

## 🆕 **v3 in one table: what the C5 replaces**

| v2 station part | Job | v3 replacement | How |
|---|---|---|---|
| **RX5808** synthesizer | Tune to each FPV channel | The C5's Wi-Fi PHY | Park the PHY on the nearest public 5 GHz Wi-Fi centre, then retune it to the exact FPV frequency (`phy_set_freq`) |
| **RX5808** RSSI pin + ADC | Received power | Raw I/Q from the PHY | The modem's diagnostic bus (MODEM_DIAG) is routed out through eight GPIO-matrix lanes and read back by the PARLIO peripheral at 40 MS/s. Power at a fixed, known receive gain becomes dBm |
| **RX5808** video out + **LM1881 / LMH1980** | Is it really video? PAL or NTSC? | Software | The same I/Q is FM-demodulated (phase step between samples) and searched for sync-tip pulses of the right width repeating at 64.0 µs (PAL) or 63.6 µs (NTSC) |
| RX5808 ±10 MHz fine-tune sweep | Carrier centre for the fingerprint | Measured carrier offset | The mean phase step of the coherent samples gives the offset from the tuned frequency to ~50 kHz |
| — | Wi-Fi rejection | FM coherence | A carrier counts only if ≥ 40 % of samples are FM-coherent (phase step within ±45°). Wi-Fi/OFDM and thermal noise fail this even when strong |

What stays: the four sector patches, the SP4T switch, the bearing estimator, the
Heltec on D4/D5, the mapper contract, the solar power design.

> ⚠️ **Status: compiles, links, and is untested on hardware.** The PHY register
> sequence, lane map and metrics are the [C5VRX](https://github.com/colonelpanichacks/c5vrx)
> project's hardware findings on ESP-IDF 6.0; this firmware re-implements them on
> the Arduino/pioarduino (ESP-IDF 5.5) framework the rest of the fleet uses. Read
> [Bench validation](#-bench-validation-the-gate-before-fielding) before
> building more than one.

---

## 🧭 **Hardware**

One v3 station is:

| Block | Part | Notes |
|---|---|---|
| Antennas | **4 × 5.8 GHz patch, 8 dBi, ~70° beam**, on the four faces of the box | One batch, matched; linear or RHCP |
| RF switch | **SP4T, 0.1–6 GHz, 3.3 V control** — SKY13322-375LF or PE42442 | Common port → the XIAO's **U.FL** antenna connector |
| Receiver + MCU | **Seeed XIAO ESP32-C5** | The receiver **is** the C5. Ships with a U.FL antenna pigtail; use the SP4T instead |
| Mesh | **Heltec WiFi LoRa 32 V3** running Meshtastic | Node name = `NODE_ID` |
| Power | 20 W panel, MPPT charger with 5 V out, 4 × 18650 (≈ 50 Wh) | Station load ≈ 1.0 W (the PHY replaces the 0.6 W RX5808 with ~0.4 W of radio) |

Gone from the v2 BOM: RX5808, sync separator, video coupling parts, the RX5808
SPI wiring. Two documents carry the full design:

- 📄 [**`docs/LEVEL1-V3-C5PHY-HARDWARE.md`**](docs/LEVEL1-V3-C5PHY-HARDWARE.md) — parts, pin budget, how the receiver works, power budget, open questions, bench log
- 🖨️ [**`docs/Level1-Station-v3-C5PHY-Bench-Guide.pdf`**](docs/Level1-Station-v3-C5PHY-Bench-Guide.pdf) — printable eight-sheet A4 bench guide: block diagram, wiring list and BOM with tick boxes, expected serial output, and the seven-stage procedure with blanks for every value you measure (source in `docs/bench-guide-src/`)

The v2 design stays documented in [`docs/LEVEL1-V2-HARDWARE.md`](docs/LEVEL1-V2-HARDWARE.md)
and its own [bench guide](docs/Level1-Station-v2-Bench-Guide.pdf).

### Pins

Use the **D-pin labels silkscreened on the XIAO**. The mesh UART is on **D4/D5**
exactly as on every other station tier, so the same carrier header fits.

| XIAO pin | C5 GPIO | v3 use | v2 used it for |
|---|---|---|---|
| **D0** | 1 | **I/Q lane — leave unconnected** | RX5808 RSSI |
| **D1** | 0 | **I/Q lane — leave unconnected** | switch V1 |
| **D2** | 25 | SP4T control **V1** | switch V2 |
| **D3** | 7 | **I/Q lane — leave unconnected** | switch V3 |
| **D4** | 23 | Heltec RX (UART TX) | same |
| **D5** | 24 | Heltec TX (UART RX) | same |
| **D6** | 11 | free (UART0 console TX) | CSYNC |
| **D7** | 12 | free (UART0 console RX) | VSYNC |
| **D8** | 8 | SP4T control **V2** | RX5808 CLK |
| **D9** | 9 | SP4T control **V3** (spare if the switch decodes 2 lines) | RX5808 CS |
| **D10** | 10 | **I/Q lane — leave unconnected** | RX5808 DATA |
| back pads GPIO2/3/4/5 | | **I/Q lanes — leave unconnected** | — |
| 3V3 | | switch VDD | |
| 5V | | from the charger's 5 V rail (also feeds the Heltec) | |
| GND | | everything, one star point | |

The **I/Q lanes** carry the PHY's raw sample bits out of the chip through the
GPIO matrix and straight back into the PARLIO peripheral. Nothing may be wired to
those pads: no pull resistors, no test points that load them. The set (GPIO
1, 0, 2, 7, 10, 5, 3, 4) is the one C5VRX proved on hardware; it is configurable
in `config.h` as GPIO numbers, not D-labels.

RF: each patch → switch RF1–RF4 through **equal-length coax ≤ 15 cm**; switch
common → the XIAO's **U.FL** connector. There is no video wiring.

> **The C5 D4/D5 = GPIO23/24 question is closed.** C5VRX's bench notes measured
> their resistor DAC on the XIAO's D4–D9 pads as GPIO 23, 24, 11, 12, 8, 9 — the
> Arduino variant's numbers, which this firmware and the v2 firmware use. The
> `remoteid-c5-5g` Level 2 firmware on `main` still hardcodes GPIO6/GPIO7 and
> needs the fix described in its README.

---

## ⚡ **Quick Start**

### What you need

The parts in the table above, a computer with VS Code and a USB-C cable, a
**5.8 GHz analog VTX you can key on demand** (nothing can be validated without one),
and the [`main`](https://github.com/tsuinami-r1/drone-mesh-5plus) branch running
`mesh-mapper.py` at the collection point.

<br>

### 1 · Install the toolchain and clone

Firmware builds with **PlatformIO in VS Code via the pioarduino IDE extension**.
Follow [Step 1 on `main`](https://github.com/tsuinami-r1/drone-mesh-5plus/blob/main/README.md#step-1--install-the-toolchain-vs-code--pioarduino)
for the extension, udev rules and the CLI alternative, then:

```bash
git clone --branch level1-c5phy https://github.com/tsuinami-r1/drone-mesh-5plus.git level1-c5phy
```

In VS Code use **File → Open Folder…** on `level1-c5phy/` — the project folder,
**not** the repository root, or PlatformIO will not find the environment.

<br>

### 2 · Wire it

Follow the [pin table](#pins). For the first bench board you need only the XIAO,
a USB cable and one patch (or the stock antenna) on the U.FL: the switch lines
can float, the firmware still scans through "sector 0". Add the SP4T and the
other three patches once the receiver itself is proven.

<br>

### 3 · Configure the station

Everything lives in `level1-c5phy/src/config.h`. Three things must be set per
station before flashing:

```cpp
#define NODE_ID              "RX01"   // unique; must equal the Meshtastic node name (≤ 6 chars keeps mesh lines short)
#define STATION_HEADING_DEG  0        // true bearing of the box face carrying patch N — measure at install
#define SECTOR_SWITCH_TABLE  { {0,0,0}, {1,0,0}, {0,1,0}, {1,1,0} }   // V1,V2,V3 per sector: FROM THE SWITCH DATASHEET
```

and one that is new in v3 and only matters if the heartbeat reports `tune_fail`:

```cpp
#define RF_COUNTRY_CC        ""       // "" = driver default ("01"); try "US"/"HK"/... if channels are refused
```

<br>

### 4 · Flash — **full erase first**

```bash
cd level1-c5phy
pio run -e seeed_xiao_esp32c5 --target erase      # mandatory before the first flash and after any PHY trouble
pio run -e seeed_xiao_esp32c5 --target upload
```

The erase clears the Wi-Fi PHY's stored calibration data. C5VRX found that stale
calibration state from a previous firmware leaves the receive chain mistuned or
deaf; every one of their flashing guides starts with `erase_flash`. Do the same.

> **ESP32-C5 will not connect?** Hold **BOOT**, tap **RESET**, release **BOOT**,
> then immediately re-run.
>
> **No toolchain?** [`firmware/`](firmware/README.md) holds a prebuilt merged
> image with the default configuration — `RX01`, heading 0, binary switch table,
> unmeasured calibration — for bench testing only.

<br>

### 5 · Verify the receiver on the bench

```bash
pio device monitor --baud 115200
```

Within a few seconds:

```json
{"info":"c5phy v3 station ready","node_id":"RX01","receiver":"c5phy","hw":"v3","channels":40,"sectors":4,"heading":0,"threshold_dbm":-87.0,"threshold_level_db":8.0,"q_min":40,"peak_pick":1,"video":1,"bw40":1,"gain_max":62,"window_us":409,"rf":true}
```

`"rf":true` means the Wi-Fi PHY came up receive-only and the PARLIO reader is
running. `"rf":false` is followed by an `{"info":"error",...}` line naming the
step that failed; the station then keeps heartbeating with `"scanning":false`.

Then use the **bench console** (type into the monitor, Enter-terminated):

| Command | Does |
|---|---|
| `?` | help + status: tuned frequency, Wi-Fi bootstrap channel, gain, capture and error counts, `tune_fail` |
| `h R3` or `h 5732` | hold that channel and print one metrics line every 200 ms |
| `s 0`…`s 3` | select a sector while holding |
| `g 30` / `g a` | fixed receive gain index / back to automatic |
| `v` | run the video check on the held channel and print every window |
| `x` | resume scanning |

With **no VTX on**, `h A1` should stream lines like

```json
{"info":"bench","freq_mhz":5865,"sector":0,"gain":62,"level_db":0.3,"rssi_dbm":-94.7,"q_phase":7,"p_mean":2.1,"p_med":2,"clip":0,"origin":700,"cfo_khz":-812,"mod":0,"noise":0,"phy_rssi":-97,"nf_dbm":-98}
```

— power near the noise reference (`p_mean` ≈ 2, `level_db` ≈ 0) and single-digit
coherence. **Key a VTX on A1** and the same line should jump: `level_db` up by
tens of dB, `gain` stepping down from 62 as the 4-bit I/Q starts clipping,
`q_phase` above 50, `mod` = 1. Then `v` should print windows with
`"video":1,"std":"NTSC"` (or PAL) and a `line_hz` near 15734 (or 15625).

Only when both halves of that work is the receiver alive; carry on to the
[bench validation](#-bench-validation-the-gate-before-fielding) list.

<br>

### 6 · Wire the Heltec and name the node

Three wires: XIAO **D4** → Heltec RX, XIAO **D5** ← Heltec TX, GND ↔ GND. Set the
Heltec's Meshtastic serial module to `TEXTMSG` at 115200 and **name the node after
`NODE_ID`**:

```bash
meshtastic --set-owner "RX01" --set-owner-short "RX01"
```

Full Meshtastic setup is [Step 5 on `main`](https://github.com/tsuinami-r1/drone-mesh-5plus/blob/main/README.md#step-5--wire-the-node-to-its-heltec-and-configure-meshtastic).

<br>

### 7 · Calibrate, then put it on the map

Do the three [calibrations](#-calibration): threshold, dBm and bearing pattern.
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
XIAO into the mapper host over USB and pick its port in the Settings panel: the
mapper needs **no change** for a v3 station (it already labels `receiver:"c5phy"`
as 5.8 GHz by frequency and prefers `rssi_dbm` over the raw count).

---

## 🧪 **Bench validation — the gate before fielding**

Everything below is a real unknown, in the order it blocks the rest. The
[printable bench guide](docs/Level1-Station-v3-C5PHY-Bench-Guide.pdf) walks the
same checks as a seven-stage procedure with blanks for every value; record the
numbers in `docs/LEVEL1-V3-C5PHY-HARDWARE.md` §7 as you go.

1. **Does the PHY come up receive-only and stream I/Q?** Boot line `"rf":true`,
   `?` shows `captures` climbing and `cap_err` at 0. If `rf_start` fails, the
   error names the ESP-IDF call; if captures fail, the PARLIO/lane path is the
   suspect (see Troubleshooting).
2. **Which channels tune?** After one sweep the heartbeat's `tune_fail` must be
   0. Each FPV channel is reached from the nearest public 5 GHz centre (5660–5885 MHz)
   that the regulatory table accepts. `tune_fail > 0` → set `RF_COUNTRY_CC`.
3. **Does a VTX register on its channel and only there?** With a VTX on R3,
   the sweep must report R3 (and possibly its ±20 MHz neighbours, which
   `PEAK_PICK` folds into one). A hit on a channel 40 MHz away means the
   coherence gate is not doing its job — check `q_phase` on that channel with `h`.
4. **Video check on a real VTX, both standards.** `v` on a held channel with an
   NTSC camera, then a PAL one. C5VRX proved NTSC recovery on hardware; **PAL is
   unproven**. Expect `line_hz` 15734 ± 10 for NTSC and 15625 ± 10 for PAL.
5. **Sensitivity and the dBm curve.** A VTX through a step attenuator (or at
   known open-field distances), `level_db` versus attenuation. The slope should
   be 1 dB/dB over ~60 dB; where the curve flattens at the bottom is the
   station's floor. Compare with an RX5808 v2 station side by side: this number
   decides whether v3 replaces v2 or only supplements it.
6. **Sector switch through the U.FL.** The SP4T's insertion loss and the switch
   truth table, as for v2, then the bearing sweep.

Two behaviours to watch for that C5VRX documents on their IDF 6.0 build and this
port could inherit: a periodic disturbance from the PHY's 1 Hz PLL tracking
timer (the firmware deinitialises it; confirm captures stay quiet), and the need
for a full flash erase whenever the receiver "goes deaf" after reflashing.

---

## 🔧 **Calibration**

Three calibrations, each one-time per station.

### Threshold — required

With no FPV transmitter powered nearby:

1. Hold any channel (`h A1`) and watch `level_db` for 30 s. It should sit near
   0 ± 2 dB on a quiet channel. Note the highest value across a few channels.
2. `DETECT_LEVEL_DB` (default 8 dB) must be well above that. Raise it if Wi-Fi
   traffic on 5.8 GHz keeps a channel high; the coherence gate rejects Wi-Fi, but
   not a constant-envelope signal.
3. Power a known VTX and confirm only its channel and its immediate neighbours
   report.

### dBm curve — required before a station joins a fleet

`rssi_dbm` is `RSSI_CAL_DBM_AT_NOISE + level_db × RSSI_CAL_SLOPE + RSSI_CAL_OFFSET_DB`.
The default (`-95`, `1.0`, `0`) is a physics estimate of the C5's noise floor in a
20 MHz bandwidth, **not a measurement**. Stations with different curves disagree
about the same signal, and the position solver reads that as noise.

1. Put a VTX on a known channel through a step attenuator at two known levels
   about 50 dB apart (a power meter on the VTX output, or a calibrated v2 station).
2. Hold the channel and note `level_db` at each.
3. Solve the two-point line for `RSSI_CAL_DBM_AT_NOISE` and `RSSI_CAL_SLOPE`.
4. Use `RSSI_CAL_OFFSET_DB` for this station's antenna and cable gain, so the
   fleet agrees on one source.
5. Reflash and check the heartbeat's `threshold_dbm` is sane (roughly −85 to −90
   with the default 8 dB threshold).

### Bearing pattern — required for bearings

Unchanged from v2: the bearing is `az[strongest] + K × (P[right] − P[left])` in
degrees, `K` in degrees per dB from the measured patch pattern.

1. Mount all four patches on the box, put a VTX at 30 m on a known bearing with clear line of sight.
2. Rotate the box in 15° steps through a full turn and log the four `sectors` values at each step.
3. Fit `K` and set `BEARING_DEG_PER_DB`; the default 3 °/dB suits an 8 dBi, 70° patch.
4. At install, measure the true bearing of the N face and set `STATION_HEADING_DEG`.

---

## ⚙️ **Configuration reference**

All in `level1-c5phy/src/config.h`.

**Station**

| Constant | Default | What it does |
|---|---|---|
| `NODE_ID` | `"RX01"` | Unique per station; must equal the Meshtastic node name. Keep it ≤ 6 characters |
| `STATION_HEADING_DEG` | `0` | True bearing of the N face; added to every reported bearing |
| `STATION_HW` / `RX_NAME` | `"v3"` / `"c5phy"` | Reported as `hw` / `receiver` in every line |

**Receiver**

| Constant | Default | What it does |
|---|---|---|
| `IQ_WINDOW_BYTES` | `16384` | Samples per capture window: 409.6 µs = 6.4 video lines |
| `RF_BW40` | `1` | Analog filter BW40 (proven) vs BW20 (+3 dB, unproven at boot) |
| `RF_GAIN_MAX` / `RF_GAIN_MIN` / `RF_GAIN_STEP_DOWN` | `62` / `2` / `12` | Forced receive gain index range and the step taken while the I/Q clips |
| `RF_CLIP_MAX_PERMILLE` | `30` | Clipped-sample share that triggers a gain step |
| `RF_GAIN_DB_PER_STEP` / `RF_NOISE_POWER` | `1.0` / `2.0` | The level formula: `(62 − gain) × step + 10 log10(p_mean / noise)` |
| `RF_TUNE_SETTLE_MS` / `RF_GAIN_SETTLE_MS` | `8` / `3` | Wait after a retune / a gain write |
| `RF_COUNTRY_CC` | `""` | Regulatory domain; `""` keeps the driver default. Set if `tune_fail > 0` |
| `SCAN_LOW_BAND` | `0` | Add L1–L8 (5362–5621 MHz); unproven, off |
| `IQ_LANE_GPIOS` / `IQ_LANE_DIAG` | C5VRX set | The eight loopback GPIOs and the MODEM_DIAG bits on them |

**Detection**

| Constant | Default | What it does |
|---|---|---|
| `WINDOWS_PER_SECTOR` / `WINDOWS_PER_SECTOR_QUICK` | `3` / `1` | Windows per sector; level = minimum (bursts rejected), coherence = median |
| `QUICK_DWELL_MISSES` / `QUICK_DWELL_RECHECK_SWEEPS` | `3` / `4` | A channel silent for 3 sweeps gets the quick count, re-checked every 4th sweep |
| `DETECT_LEVEL_DB` | `8.0` | Level above the noise reference a hit needs |
| `DETECT_Q_PHASE_MIN` | `40` | % of FM-coherent samples a hit needs |
| `RSSI_CAL_DBM_AT_NOISE` / `RSSI_CAL_SLOPE` / `RSSI_CAL_OFFSET_DB` | `−95` / `1.0` / `0.0` | The level → dBm line |

**Sectors, bearing, video, reporting**

| Constant | Default | What it does |
|---|---|---|
| `SECTOR_AZIMUTHS`, `SECTOR_SWITCH_TABLE`, `SECTOR_SETTLE_US` | as v2 | Patch boresights, switch truth table, settle |
| `BEARING_*` | as v2 | Pattern slope, clamp, sigma |
| `VIDEO_WINDOWS` / `VIDEO_WINDOW_GAP_MS` | `8` / `5` | Windows demodulated per peak and their spacing |
| `VIDEO_MIN_QUALITY` / `VIDEO_MIN_WINDOWS` | `70` / `3` | A window counts with sync score ≥ 70; the carrier is video when ≥ 3 windows count |
| `VIDEO_MAX_PER_SWEEP` | `2` | Peaks that get a video check per sweep |
| `ENABLE_MESH_RELAY`, `*_INTERVAL_MS`, `PEAK_PICK`, `PEAK_WINDOW_MHZ`, `MESH_LINE_MAX` | as v2 | Reporting cadence and limits |
| `BENCH_CONSOLE` | `1` | The USB bench console; `0` removes it |

<details>
<summary><b>Firmware layout</b></summary>

<br>

```
level1-c5phy/
  platformio.ini            one env: seeed_xiao_esp32c5 (pinned platform, see below)
  LICENSE                   GPL-3.0-only (derived from C5VRX)
  src/
    config.h                every pin and tuneable
    main.cpp                sweep (every channel × every sector), peak-pick, video check on the
                            strongest peaks, JSON, relay, heartbeat, bench console
    c5phy_rf.*              Wi-Fi PHY as a receive-only front end: init, TX queues off, retune,
                            MODEM_DIAG lane routing, fixed gain, BW40
    iq_capture.*            PARLIO RX one-shot capture of a 16 KiB I/Q window
    demod.*                 power / FM-coherence metrics; software FM demod + H-sync search
    sector_switch.*         SP4T control lines and sector azimuths (from v2)
    bearing.*               amplitude-comparison bearing + sigma (from v2)
```

Per channel visit: retune (~10 ms), then per sector 1–3 windows of 0.4 ms each
plus ~2 ms of metrics, with a gain step and re-capture while the I/Q clips.
A full 40-channel sweep is about 2 s; each due peak then gets 8 windows over
~100 ms for the video check.

**Toolchain pin.** `platformio.ini` pins pioarduino 55.03.39 because
`c5phy_rf.cpp` links undocumented PHY symbols (`phy_set_freq`,
`phy_force_rx_gain`, `phy_disable_agc`, `phy_rfagc_disable`, `phy_wifi_fbw_sel`,
`phy_get_rssi`, `phy_get_noise_floor` from `libphy.a`; `phy_track_pll_deinit`
from `libesp_phy.a`; `lmac_stop_hw_txq` from `libpp.a`). They are strong
references on purpose: a platform bump that drops one fails at link time
instead of shipping a receiver that cannot tune.

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

Raceband R7 and Band F8 are both 5880 MHz. Band A sits exactly on Wi-Fi
channels 149–173 and needs no undocumented retune at all; every other channel is
reached from the nearest of those centres (or 5660–5720 MHz for the low E and R
channels). `SCAN_LOW_BAND` adds L1–L8.

</details>

---

## 🔌 **Mapper contract**

The interface between the branches: `mesh-mapper.py` on `main` consumes exactly
these lines, over USB or through the home node from the mesh. **A v3 station
emits the v2 contract**, with `receiver:"c5phy"`, `hw:"v3"` and a few extra
diagnostic keys that the mapper stores and ignores.

> ⚠️ **Rule for contributors:** a change to any emitted key lands together with
> the matching `mesh-mapper.py` change on `main`, and the commit message names
> the `main` commit. Stations in the field are not reflashed when the mapper
> updates, so the mapper keeps accepting older keys.

### Detection line (USB)

```json
{
  "type":     "analog_fm",
  "receiver": "c5phy",
  "hw":       "v3",
  "mac":      "AF:00:16:6C:52:03",
  "freq_mhz": 5732,
  "band":     "R",
  "ch":       3,
  "rssi_raw": 903,
  "rssi":     903,
  "rssi_dbm": -68.1,
  "rssi_min": 890,
  "rssi_max": 915,
  "rssi_n":   3,
  "level_db": 26.9,
  "gain":     50,
  "q_phase":  71,
  "cfo_khz":  1840,
  "carrier":  "fm",
  "sectors":  [-68.1, -74.4, -91.0, -85.2],
  "sector":   0,
  "bearing_deg": 32,
  "bearing_sigma_deg": 15,
  "freq_peak": 5734,
  "video":    "NTSC",
  "sync_hz":  15736,
  "field_hz": 60,
  "sync_q":   88,
  "sync_score": 91,
  "video_windows": 8,
  "fp":       "NTSC/15736/5734",
  "basic_id": "5.8G-R3-5732MHz",
  "node_id":  "RX01",
  "seq":      42
}
```

| Key | Present | What it means on v3 |
|-----|---------|---------------------|
| `type`, `mac`, `node_id`, `freq_mhz`, `band`, `ch`, `basic_id`, `seq` | as v2 | Unchanged |
| `rssi_dbm` | **always** | Calibrated power on the strongest sector, from the level formula. Ring radius, ring colour and the multi-station solver |
| `rssi_raw`, `rssi`, `rssi_min`, `rssi_max` | USB | **Synthesised**: `rssi_dbm` pushed back through the mapper's own RX5808 curve, so a mapper without `rssi_dbm` support still draws the right ring. Min/max are the spread across the windows; `rssi_n` is the window count |
| `level_db`, `gain` | USB, new | dB above the noise reference, and the gain index it was measured at |
| `q_phase`, `cfo_khz`, `carrier` | USB, new | % FM-coherent samples; carrier offset from the tuned frequency; `fm` (modulated, constant envelope), `cw` (unmodulated) or `noise` |
| `sectors`, `sector`, `bearing_deg`, `bearing_sigma_deg` | as v2 | dBm per sector, strongest, bearing and 1σ |
| `freq_peak` | when measured | Tuned frequency + `cfo_khz`, rounded to MHz |
| `video`, `sync_hz`, `sync_q` | when measured | `NTSC`, `PAL` or `none`; line rate from the measured line period; % of windows with a sync train |
| `field_hz` | when measured | **Nominal** for the detected standard (50/60). v3 does not measure the field rate; v2 does |
| `sync_score`, `video_windows` | USB, new | Mean sync score of the windows that counted; windows demodulated |
| `fp` | when video present | `<standard>/<sync_hz>/<freq_peak>` — the per-drone fingerprint |

### Detection line (mesh relay)

Byte-for-byte the v2 format, at most every `MESH_REPORT_INTERVAL_MS`:

```json
{"type":"analog_fm","mac":"AF:00:16:6C:52:03","freq_mhz":5732,"band":"R","ch":3,"rssi_dbm":-68.1,"bearing_deg":32,"bearing_sigma_deg":15,"fp":"NTSC/15736/5734","node_id":"RX01","seq":42}
```

It **must be JSON** and **must stay under 200 bytes**; the firmware drops the
fingerprint, then the bearing, rather than ever truncating. The mapper backfills
`rssi`, `basic_id` and `receiver` (as `rx5808`, a label only).

### Heartbeat and info lines

Every 60 s on USB and every 120 s on the mesh, plus once at boot:

```json
{"heartbeat":true,"node_id":"RX01","receiver":"c5phy","hw":"v3","scanning":true,"channels":40,"sectors":4,"heading":0,"threshold":1010,"threshold_dbm":-87.0,"video_seen":7,"gain_max":62,"bw40":1,"tune_fail":0,"cap_err":0,"sweeps":1830,"nf_dbm":-98,"temp_c":41.2,"uptime_s":3600,"seq":42}
```

New on v3: `scanning` is false when the PHY failed to start; `tune_fail` counts
channels the regulatory table refused; `cap_err` counts failed I/Q captures;
`nf_dbm` is the PHY's own noise-floor estimate when it returns one. The mesh copy
is the v2 subset: `node_id`, `receiver`, `hw`, `heading`, `threshold_dbm`,
`video_seen`, `temp_c`, `uptime_s`, `seq`.

### What the mapper does with several stations

Unchanged: reports from two or more positioned stations within 20 MHz and the
fusion window are solved for position and transmitter power, silent-but-alive
stations constrain it, fixes go to ATAK/WinTAK. `GET /api/analog_fixes`,
`GET /api/analog_nodes`, `GET/POST /api/analog_fusion` on `main`.

---

## 📦 **The v2 station on this branch**

`level1-analog-fpv/` is the RX5808 + sync-separator station exactly as on
[`level1-station`](https://github.com/tsuinami-r1/drone-mesh-5plus/tree/level1-station),
which remains its README, and `docs/LEVEL1-V2-HARDWARE.md` and the bench guide PDF
are its design. It builds for both boards from the same source:

```bash
cd level1-analog-fpv && pio run -e seeed_xiao_esp32s3 -e seeed_xiao_esp32c5
```

Until the v3 [bench validation](#-bench-validation-the-gate-before-fielding) is
through, v2 is the design to field.

---

## 🗺️ **Roadmap**

| Next | What it adds | Status |
|---|---|---|
| **Bench validation of v3** | The six checks above, PAL included; sensitivity against an RX5808 | 📋 Blocks everything else on this branch |
| **v3 carrier PCB** | XIAO C5 footprint, SP4T with four U.FL launches, UART header, nothing else | 📋 After validation |
| **Mapper consumes v2/v3 keys** (`main`) | Bearing residuals in the solver; `fp` as a second clustering key | 📋 Next `main` change |
| **Field rate on v3** | Timed captures across a field to measure `field_hz` instead of inferring it | 📋 Nice to have |
| **[RX3364 3.3 GHz](docs/RX3364-INTEGRATION-PLAN.md)** | Long-range analog on 3.3 GHz — the C5's radio cannot reach it, so this stays a v2-style module | 📋 Blocked on gate 0 |

---

## 🤝 **Contributing**

Start here, then read [`CLAUDE.md`](CLAUDE.md) — short, and it holds the rules
that are easy to break by accident.

**The four that matter most:**

1. **This branch is firmware only.** `mesh-mapper.py`, the Level 2 firmware and the home node live on `main` and are never copied here.
2. **v3 is C5-only; v2 builds for both boards, every time.** `level1-c5phy/` refuses any board but the XIAO ESP32-C5 by design. `level1-analog-fpv/` must keep building and behaving on the S3 and the C5.
3. **The I/Q lanes and the PHY symbols are pinned.** Do not move the loopback GPIOs, the mesh UART off D4/D5, or the platform version without a bench check.
4. **Changing an emitted JSON key changes the contract.** The matching `mesh-mapper.py` change lands on `main` in the same change set.

**Before you commit:**

```bash
cd level1-c5phy      && pio run -e seeed_xiao_esp32c5                            # no warnings in src/
cd ../level1-analog-fpv && pio run -e seeed_xiao_esp32s3 -e seeed_xiao_esp32c5   # v2 untouched, both boards
```

---

## 🐛 **Troubleshooting**

<details open>
<summary><b><code>"rf":false</code> at boot</b></summary>

- The `{"info":"error"}` line names the ESP-IDF call. `esp_wifi_set_bandwidths(BW40)` failing means the driver refused 40 MHz on 5 GHz — check the platform version; the firmware has no BW20 fallback on purpose.
- `no 5 GHz channel accepted by regulatory table`: set `RF_COUNTRY_CC`.
- `MAC TX queues still enabled`: a register layout change; this is IDF-pinned code.

</details>

<details>
<summary><b>Captures fail (<code>cap_err</code> climbing) or every channel reads the same</b></summary>

- Something is loading an I/Q lane pad (D0, D1, D3, D10 or the back pads). They must float.
- `p_mean` stuck at 0 with `origin` = 1000: the lanes are not carrying the modem bus; the PARLIO input or the MODEM_DIAG routing is not in place. `rf_route_iq_lanes()` runs after `iq_init()` for exactly this reason.
- Identical `p_mean` on every channel with a VTX on: the retune is not taking; check `wifi_ch` in `?` changes between channels.

</details>

<details>
<summary><b>A VTX is on but no channel ever hits</b></summary>

- `h` the channel: if `level_db` rises but `q_phase` stays low, the carrier is not FM-coherent as seen here — it may be off-channel (check `cfo_khz`), or the I/Q lane order is wrong (bits scrambled). If `level_db` does not rise either, the receiver is deaf: full erase and reflash.
- `mod` = 0 with a strong carrier: an unmodulated source; the detector still hits, `carrier` reports `cw`.

</details>

<details>
<summary><b><code>video</code> is always <code>none</code> on a real VTX</b></summary>

- `v` shows every window: `swing` must be well above 64 (there is FM deviation), `pulses` > 0 (sync-shaped dips found), `periods` ≥ 2 (they repeat at a line period). Which of those is zero says where the chain breaks.
- Windows with `pulses` but `periods` = 0: the line period is outside 62–65.5 µs; check `period` in the output.
- Only PAL fails: PAL is unproven on this receiver — report the windows.

</details>

<details>
<summary><b>Bearings, positions, mesh problems</b></summary>

- Same as v2: switch table from the datasheet, `STATION_HEADING_DEG` measured, Meshtastic node name equals `NODE_ID`, `POST /api/node_location` by hand if needed, dBm calibration on every station in a fleet.

</details>

<details>
<summary><b>Build fails</b></summary>

- Open `level1-c5phy/` in PlatformIO, not the repository root.
- `#error … supports the Seeed XIAO ESP32-C5 only`: you selected another board.
- `undefined reference to phy_set_freq` (or another `phy_*`): the platform is not the pinned one; delete `.pio/` and rebuild, or re-check the symbols in the new `libphy.a` before bumping the pin.

</details>

---

## 📄 **License**

The repository is MIT, as is the upstream project it is derived from.
**`level1-c5phy/` is GPL-3.0-only**: its PHY front end, capture path and metrics
derive from [C5VRX](https://github.com/colonelpanichacks/c5vrx), which is
distributed under GPL-3.0-only. The two licences live side by side because the
two firmware projects are separate programs; do not copy code from
`level1-c5phy/` into the MIT-licensed projects.

## 🙏 **Acknowledgments**

- **ColonelPanic** and **Luke Switzer** — the original [drone-mesh-mapper](https://github.com/colonelpanichacks/drone-mesh-mapper)
- **C5VRX** (Twotoz, ItsReckliss and contributors) — the discovery that the ESP32-C5's Wi-Fi PHY receives analog 5.8 GHz video, and every register on which v3 stands
- **"Alik",** 93rd OMBr, and **"Ivan",** 427th Rarog, Armed Forces of Ukraine
- **pioarduino** — the maintained Arduino-ESP32 platform for PlatformIO that makes the C5 build possible
- **RotorHazard / Chorus** — the RTC6715 synthesizer register formula (v2)
