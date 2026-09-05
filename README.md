# <div align="center">  **Remote Drone Mapper** </div>

<div align="center">

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Python](https://img.shields.io/badge/Python-3.7+-blue.svg)](https://www.python.org/)
[![ESP32](https://img.shields.io/badge/ESP32-S3%20%7C%20C5-green.svg)](https://www.espressif.com/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-pioarduino-orange.svg)](https://github.com/pioarduino/platform-espressif32)

**A private, discreet, solar-powered counter-surveillance network for drone detection.**

Real-time Remote ID, DJI DroneID, MAVLink & analog-FPV mapping over a Meshtastic-linked mesh of unattended ESP32 nodes

[🚀 Quick Start](#-quick-start) • [📦 Firmware](#-firmware-variants) • [🔧 Hardware](#-hardware-setup) • [🛠️ API Reference](#️-api-reference)

<img src="eye.png" alt="Drone Detection Eye" style="width:50%; height:25%;">

</div>

---

## 🎯 **Overview**

Special thanks to "Alik", 93rd OMBR; and "Ivan", 427th UAS Brigade, Armed Forces of Ukraine, for their help.
Slava Ukraini.

This is a hardened fork of
[colonelpanichacks/drone-mesh-mapper](https://github.com/colonelpanichacks/drone-mesh-mapper)
(original code by Luke Switzer and ColonelPanic), reworked for a multi-node,
city-scale deployment on a mix of XIAO ESP32-S3 and ESP32-C5 boards. On top of the
upstream Remote ID mapper it adds:

- **DJI DroneID** and **MAVLink** decoding in every firmware variant
- **Full-band channel scanning** (all 13 × 2.4 GHz channels, plus 5 GHz UNII-3 on the C5)
  with hit-triggered dwell, instead of a fixed channel
- **XIAO ESP32-C5 dual-band** support, built on the [pioarduino](https://github.com/pioarduino/platform-espressif32)
  platform (Arduino-ESP32 core 3.x)
- **RX5808 analog FPV detection** nodes with range rings on the map
- **TAK / ATAK / WinTAK** Cursor-on-Target output and inbound operator positions
- Unattended-operation hardening: watchdog resets, serial reconnect loops, bounded
  over-the-air parsers, per-drone mesh rate limiting, and filtering so serial-side
  noise can never be broadcast onto the mesh

---

## 🛰️ **Intended use & deployment model**

This is a **private, discreet counter-surveillance drone-detection network,** not a
single sensor on a bench. The goal is persistent, low-profile awareness of drone
activity over an area you care about (a property, a site, an event), built from a mesh
of **unattended, self-powered field nodes**.

**The counter-surveillance job:** answer *"is something watching this area, and from
where?"* Each detection is mapped in real time, and where the protocol exposes it, so
is the **operator / pilot location** — turning a drone overflight into an actionable
picture of who is flying and from where, without ever tipping them off.

**How it is meant to be deployed:**

- **Distributed, receive-only nodes.** Each node is a XIAO ESP32 (S3 or C5) that
  *passively* listens for drone Remote ID, DJI DroneID, MAVLink, and analog FPV video.
  Nodes **transmit nothing over the air to detect a drone**, so the network itself
  stays quiet, low-power, and hard to spot. Discretion is a design property, not an
  afterthought.
- **Meshtastic backhaul — no infrastructure needed.** Each node pairs with a Heltec
  LoRa (Meshtastic) radio over UART. Detections travel home over the **encrypted LoRa
  mesh.** No Wi-Fi, no cellular, no internet backhaul. This is what lets the network
  blanket a wide area, including places with no power or connectivity.
- **Remote, unattended, solar-powered.** Nodes run standalone in the field; the
  reference build is **solar powered**. There is no operator at the node — it wakes,
  listens, relays over the mesh, and keeps running. The firmware is hardened for
  continuous unattended operation (watchdog reset, serial reconnect loops, bounded
  over-the-air parsers, per-node mesh rate-limiting).
- **One collection point.** A single machine (a laptop or Raspberry Pi) runs
  `mesh-mapper.py`, ingests the mesh via a paired "home" node on USB, and serves the
  live map, detection history, and TAK/ATAK feed. Only this endpoint needs an operator;
  everything upstream is autonomous.

**Scale:** a multi-node, city-wide mesh running a mix of XIAO ESP32-S3 and ESP32-C5
detection nodes simultaneously, each named to match its paired Meshtastic node.

### Protocol coverage

| Protocol | Transport | OUI / identifier | Firmware support |
|----------|-----------|-----------------|-----------------|
| ASTM F3411 / OpenDroneID | 802.11 beacon (IE 221) | `FA:0B:BC` | All variants |
| ASD-STAN 4709-002 | 802.11 beacon (IE 221) | `90:3A:E6` | All variants |
| ASTM F3411 / OpenDroneID | Wi-Fi NAN action frame | `org.opendroneid.remoteid` hash | All variants |
| ASTM F3411 / OpenDroneID | Bluetooth 4 / 5 advertising (Coded PHY on C5 and NimBLE S3 builds) | Service data `0xFFFA` | All variants |
| **DJI DroneID** | **802.11 beacon (IE 221)** | **`26:37:12`** | **All variants** |
| DJI OcuSync / O3 / O4 | OFDM (~2.4295 GHz video band) | — | ❌ Requires SDR |
| **MAVLink GPS** | **802.11 data frame (UDP/14550)** | **any MAC** | **All variants** |
| **Analog FPV video** | **5.645–5.945 GHz FM** | **synthetic `AF:00:…` MAC** | **`rx5808-detection`** |

> **DJI Wi-Fi coverage caveat:** Modern DJI aircraft (Mini 3 Pro, Air 3, Mavic 3 series) primarily use OcuSync/O3/O4 for their DroneID downlink, which is OFDM in the video band and **cannot be demodulated by an ESP32**. The Wi-Fi IE221 DroneID broadcast (`26:37:12`) is present on older and budget models; treat it as partial fleet coverage, not "all DJI".

> **MAVLink Wi-Fi coverage caveat:** Only detects ArduPilot/PX4 aircraft that broadcast unencrypted MAVLink telemetry over an open Wi-Fi AP (UDP port 14550). DJI and other consumer drones do not use MAVLink. The ESP32 listens on one channel at a time, but hops the full band (see [Channel scanning](#-channel-scanning)), so aircraft are found within one sweep rather than only on a fixed channel.

---

## 📡 **Channel scanning**

A Wi-Fi radio hears one channel at a time, so *which* channels get scanned, and how
often each is revisited, sets the real detection coverage.

### Two-tier schedule

Scanning only the consumer 1/6/11 convention leaves 10 of the 13 permitted 2.4 GHz
channels unscanned — drones pick channels dynamically and are not bound to it. But
sweeping all 13 at equal dwell pushes the revisit interval to ~2.6 s, too slow to
assemble a full OpenDroneID message set (BasicID / Location / System arrive in
separate frames).

So each cycle visits **every primary channel plus one secondary**, round-robin:

| Board | Primary (every cycle) | Secondary (one per cycle) | Dwell | Cycle | Secondary revisit |
|---|---|---|---|---|---|
| XIAO ESP32-S3 | 1, 6, 11 | 2–5, 7–10, 12, 13 | 200 ms | ~800 ms | ~8 s |
| XIAO ESP32-C5 | 1, 6, 11 + 149–165 (UNII-3) | 2–5, 7–10, 12, 13 | 50 ms | ~450 ms | ~4.5 s |

Common channels keep a sub-second revisit; every other channel is still swept within
a few seconds instead of never.

### Adaptive dwell

After a **confirmed detection** (not ordinary Wi-Fi traffic), the node lingers on that
channel so the rest of the drone's message set can be collected before hopping away —
`CHANNEL_HOLD_MS`, scaled to the dwell (C5 300 ms, S3 1200 ms) and hard-capped by
`CHANNEL_HOLD_MAX_MS` so one busy channel cannot starve the schedule. Fast-revisit
boards need less hold because they come back sooner.

### Regulatory domain

The ESP-IDF default Wi-Fi country stops at channel 11 and **silently rejects** anything
above it, so each detection firmware sets its own country code at the top of
`main.cpp`:

```cpp
#define WIFI_COUNTRY_CC   "HK"   // US/FCC: "US", 1, 11   EU/HK: 1, 13   Japan: 1, 14
#define WIFI_CHAN_START   1
#define WIFI_CHAN_COUNT   13
```

Nodes are receive-only, so this has no transmit implications — but set it to match your
jurisdiction before building. Prefer over-declaring: a channel the hardware refuses is
skipped and logged (`channel N rejected by regulatory domain`), while a channel excluded
here is never scanned at all.

---

## ⚡ **Quick Start**

The whole path from a bare board to a dot on the map. Firmware is built with
**PlatformIO inside VS Code, via the pioarduino extension**; the mapper is a single
Python script.

### What you need

| Role | Hardware | Quantity |
|------|----------|----------|
| Detection node | Seeed **XIAO ESP32-C5** (dual-band, preferred) or **XIAO ESP32-S3** | one per site |
| Field mesh radio | **Heltec WiFi LoRa 32 V3** running Meshtastic, wired to each detection node | one per node |
| Home receiver | one more Heltec V3 + one XIAO ESP32-S3 flashed as the `home_node` UART bridge | 1 |
| Host | Any Linux / macOS / Windows machine or Raspberry Pi with Python 3.7+ | 1 |
| Optional | **RX5808** 5.8 GHz receiver module for analog FPV nodes | per FPV node |
| Build machine | A computer with VS Code and a USB-C cable | 1 |

### Step 1 — Install the toolchain: VS Code + pioarduino

Use **PlatformIO running inside VS Code through the pioarduino IDE extension**. Every
`platformio.ini` in this repo pins the
[pioarduino `platform-espressif32`](https://github.com/pioarduino/platform-espressif32)
release (`55.03.39`), because the XIAO ESP32-C5 and the NimBLE / Coded PHY builds need
Arduino-ESP32 core 3.x, which the stock PlatformIO `espressif32` platform never
adopted. The pioarduino extension ships a PlatformIO core that is tested against that
platform, so it is the path of least resistance. Arduino IDE is not supported for
these projects.

1. Install [Visual Studio Code](https://code.visualstudio.com/).
2. Open the Extensions view (`Ctrl+Shift+X` / `Cmd+Shift+X`), search for
   **`pioarduino`**, and install **pioarduino IDE** (publisher *pioarduino*).
   If the official *PlatformIO IDE* extension is already installed, disable it
   first — both register the same commands and toolbar.
3. Wait for the status bar to finish "Installing PlatformIO Core". Reload VS Code when
   prompted. A PlatformIO alien-head icon appears in the activity bar.
4. **Linux only:** install the udev rules so the XIAO enumerates without root, then
   add yourself to the serial group and log out/in:
   ```bash
   curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
     | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
   sudo udevadm control --reload-rules && sudo udevadm trigger
   sudo usermod -aG dialout $USER     # 'uucp' on Arch
   ```
5. Clone this repo:
   ```bash
   git clone https://github.com/tsuinami-r1/drone-mesh-5plus.git
   ```

> **Command-line alternative.** The same projects build headless with the PlatformIO
> CLI (`pipx install platformio` or `pip install platformio`), including on a
> Raspberry Pi. The `pio run` commands below are the CLI equivalents of each VS Code
> action, and the platform pin in `platformio.ini` means no extra setup is needed.

### Step 2 — Pick a firmware and open it

Open the **firmware project folder**, not the repo root: **File → Open Folder…** and
choose one of the directories below. PlatformIO looks for `platformio.ini` in the
workspace root, so opening the repo root shows no build targets.

| Board | Open this folder | Environment | Notes |
|-------|------------------|-------------|-------|
| XIAO ESP32-C5 | `remoteid-c5-5g/` | `seeed_xiao_esp32c5` | Dual-band 2.4 + 5 GHz, BLE 5 Coded PHY. **Preferred node.** |
| XIAO ESP32-S3 | `remoteid-c5-5g/` | `seeed_xiao_esp32s3` | 2.4 GHz, NimBLE + Coded PHY. **Preferred S3 build.** |
| XIAO ESP32-S3 (home receiver) | `node-mode-dualcore/` | `home_node` | UART→USB bridge with dedup, no detection. Plugs into the host. |
| XIAO ESP32-S3 or C5 + RX5808 | `rx5808-detection/` | `seeed_xiao_esp32s3` / `seeed_xiao_esp32c5` | Analog FPV sweep. See [RX5808](#-rx5808-58ghz-analog-fm-detection-rx5808-detection). |

The remaining projects (`remoteid-mesh`, `remoteid-mesh-dualcore`, the
`node-mode-dualcore` `remote_node` env) are older S3-only detection builds that are
kept working; see [Firmware variants](#-firmware-variants).

The first time a project opens, pioarduino downloads the platform, toolchains and
libraries (several hundred MB). Let it finish before building.

### Step 3 — Set the compile-time options

These are `#define`s, so they must be set **before** building — a prebuilt binary
cannot be changed afterwards.

- **Regulatory domain** (every detection `main.cpp`, near the top): set
  `WIFI_COUNTRY_CC`, `WIFI_CHAN_START`, `WIFI_CHAN_COUNT` for your jurisdiction. See
  [Regulatory domain](#regulatory-domain).
- **`NODE_ID`** (`rx5808-detection/src/main.cpp`): unique per RX5808 node, and it
  **must equal the paired Meshtastic node's shortName/longName** for range rings to
  resolve. Wi-Fi/BLE detection nodes derive their `node_id` from the chip MAC and need
  no edit.
- **`ENABLE_MESH_RELAY`** (`rx5808-detection/src/main.cpp`): set to `0` for a
  USB-only RX5808 node with no Heltec attached.

### Step 4 — Build, flash, verify

Plug the XIAO in over USB-C, then in VS Code:

1. Click the PlatformIO icon in the activity bar → **Project Tasks** → expand the
   environment for your board (e.g. `seeed_xiao_esp32c5`).
2. **General → Build** to compile. Fix any errors before flashing.
3. **General → Upload** to flash. PlatformIO auto-detects the port; with several
   boards attached, add `upload_port = /dev/ttyACM0` (or `COM7`) to that env in
   `platformio.ini`.
4. **General → Monitor** to open the serial monitor. `monitor_speed = 115200` is
   already set in every project.

CLI equivalents, run from inside the project folder:

```bash
pio run -e seeed_xiao_esp32c5                    # build
pio run -e seeed_xiao_esp32c5 --target upload    # build + flash
pio device monitor                               # serial monitor (115200)
```

Within a few seconds of boot the monitor should show a banner naming the board and
mode. The `remoteid-c5-5g` build then prints its UART pins and scan schedule:

```
UART:  TX=GPIO6, RX=GPIO7 → Heltec
[SCAN] Channel hopping: 8 primary + 10 secondary, dwell 50 ms
```

(`TX=GPIO5, RX=GPIO6`, `3 primary` and `dwell 200 ms` on an S3.) An RX5808 node prints
`{"info":"RX5808 scanner ready", ...}` instead. Any
`channel N rejected by regulatory domain` line means Step 3 was skipped.

> **ESP32-C5 boot mode:** if the upload fails to connect, hold **BOOT**, tap
> **RESET**, release **BOOT**, then re-run Upload. The S3 does not need this.

> **No toolchain at all?** `firmware/` holds prebuilt default-configuration
> binaries for all eight targets and `esptool` flash commands; see
> [`firmware/README.md`](firmware/README.md). Remember the defaults above are baked in.

### Step 5 — Wire the node to its Heltec and configure Meshtastic

Three wires between the XIAO and the Heltec V3: XIAO **D4** (TX) → Heltec RX, XIAO
**D5** (RX) ← Heltec TX, GND ↔ GND. The same D4/D5 header pins are used on the S3 and
the C5, so one carrier PCB serves both; the firmware picks the right GPIOs at compile
time (see the [wiring table](#wiring-for-mesh-integration)). Power the Heltec from its
own supply or the XIAO's 3.3 V/5 V pin as your build dictates.

Flash stock [Meshtastic](https://meshtastic.org/) to the Heltec, then enable the
**Serial Module** in text-message mode on the pins you wired (the reference build uses
GPIO 19 / 20 on the Heltec):

```bash
meshtastic --set serial.enabled true \
           --set serial.mode TEXTMSG \
           --set serial.baud BAUD_115200 \
           --set serial.rxd 19 --set serial.txd 20
```

Then **name the Meshtastic node.** Set its shortName (and/or longName) to the node's
identity — for RX5808 nodes exactly the firmware `NODE_ID`, e.g. `RX01`:

```bash
meshtastic --set-owner "RX01" --set-owner-short "RX01"
```

`mesh-mapper.py` resolves a node's GPS by matching this name first and only falls back
to the first GPS-equipped node in the mesh, so in a dense mesh a wrong name draws the
range ring in the wrong place. All Heltecs must share the same channel and key.

### Step 6 — Set up the home end

1. Flash a XIAO ESP32-S3 with `node-mode-dualcore` → `home_node` (Steps 2–4).
2. Wire it to the home Heltec (D4/GPIO5 → Heltec RX, D5/GPIO6 ← Heltec TX, GND) and
   configure that Heltec's serial module exactly as in Step 5.
3. Plug the home XIAO into the host machine over USB. It forwards deduplicated JSON
   detections from the mesh to USB, and only forwards lines prefixed `MESH:` in the
   other direction, so nothing the host emits can leak onto the mesh.

An RX5808 node or a detection node can also be plugged **directly** into the host by
USB for bench testing; the mapper reads the same JSON from either source.

### Step 7 — Install and run the mapper

```bash
cd drone-mesh-5plus
python3 -m pip install -r requirements.txt
python3 mesh-mapper.py
```

Open **http://localhost:5000** (the server binds all interfaces, so any machine on the
LAN can reach it on the host's IP). In the Settings panel select the home node's USB
serial port; the mapper also auto-connects to remembered ports on later runs. For a
Raspberry Pi collection point, clone the repo onto the Pi and add an `@reboot` cron
entry that runs the command above.

> The `RPI/` installer scripts are inherited from upstream and download upstream's
> `mesh-mapper.py`, not this fork's. Use the clone above instead.

### Step 8 — Smoke-test without hardware

`mapper_test/mapper_test.py` simulates five drones and posts detections to the
running mapper over HTTP, which is enough to verify the map, trails, history and
exports before any node is in the field:

```bash
python3 mapper_test/mapper_test.py --host 127.0.0.1 --port 5000 --duration 5
```

---

## 📦 **Firmware variants**

All detection variants share the same parsers (`opendroneid.c`, `odid_wifi.h`,
`bt_odid.h`, `dji_droneid.h`, `mavlink_wifi.h`) and emit the same JSON schema, so
`mesh-mapper.py` treats them identically.

| Project | Envs | Boards | Platform | What it is |
|---------|------|--------|----------|------------|
| `remoteid-c5-5g/` | `seeed_xiao_esp32c5`, `seeed_xiao_esp32s3` | C5, S3 | pioarduino 55.03.39 | **Current mainline.** Dual-band on C5, NimBLE + Coded PHY on both, full channel hopping. |
| `node-mode-dualcore/` | `remote_node`, `home_node` | S3 | pioarduino 55.03.39 | Field node + the **home UART bridge** with 500 ms multi-node dedup. |
| `remoteid-mesh-dualcore/` | `seeed_xiao_esp32s3` | S3 | pioarduino stable | S3 detection with Wi-Fi and BLE on separate cores. Classic BLE. |
| `remoteid-mesh/` | `seeed_xiao_esp32s3`, `seeed_xiao_esp32c3` | S3, C3 | stock `espressif32` | Original single-core build. C3 has no BLE Remote ID. Legacy. |
| `rx5808-detection/` | `seeed_xiao_esp32s3`, `seeed_xiao_esp32c5` | C5, S3 | pioarduino 55.03.39 | Analog 5.8 GHz FPV sweep with optional Heltec relay. |

Prebuilt binaries for every env, the binary→env mapping, and `esptool` flashing are
documented in [`firmware/README.md`](firmware/README.md). The node-mode home/remote
pair and its dedup engine are described in
[`node-mode-dualcore/README.md`](node-mode-dualcore/README.md).

---

## 🚀 **Running the mapper**

```bash
python3 mesh-mapper.py [OPTIONS]
```

| Option | Description | Default |
|--------|-------------|---------|
| `--headless` | Run without web interface | false |
| `--debug` | Enable debug logging | false |
| `--web-port PORT` | Web interface port | 5000 |
| `--port-interval SECONDS` | Serial port monitoring interval | 10 |
| `--no-auto-start` | Disable automatic connection to remembered ports | false |
| `--no-tak` | Disable TAK/ATAK CoT multicast output and receiver | false |
| `--tak-addr ADDR` | TAK multicast address | 239.2.3.1 |
| `--tak-port PORT` | TAK multicast port | 6969 |

```bash
python3 mesh-mapper.py                          # web UI on :5000, TAK multicast on
python3 mesh-mapper.py --headless --debug       # dedicated collection point, verbose log
python3 mesh-mapper.py --web-port 8080 --no-tak # custom port, no CoT output
```

**What you get:** a live Leaflet map with drone and pilot markers and trails,
persistent detections across restarts, a no-GPS panel for detections without a fix,
device aliases, FAA registration lookup for Remote ID serials, CSV / KML / GeoJSON
export, a cumulative detection log, webhook callbacks, and a Cursor-on-Target feed
that ATAK / WinTAK / iTAK pick up over multicast. Runtime output lands in
`mapper.log`, `cumulative_detections.csv`, `cumulative.kml` and per-session
`detections_*.csv|kml` files next to the script (all git-ignored).

---

## 🔧 **Hardware Setup**

### Supported boards

| Board | Status | Notes |
|-------|--------|-------|
| **Seeed XIAO ESP32-C5** | ✅ Primary | Dual-band 2.4 + 5 GHz Wi-Fi 6, BLE 5 with Coded PHY long range. Single RISC-V core. |
| **Seeed XIAO ESP32-S3** | ✅ Primary | 2.4 GHz Wi-Fi, BLE 5 (Coded PHY in the NimBLE build), dual core, PSRAM. |
| **Seeed XIAO ESP32-C3** | ⚠️ Legacy | `remoteid-mesh` only. Single core, Wi-Fi only, no BLE Remote ID. |
| **Heltec WiFi LoRa 32 V3** | ✅ Mesh radio | Runs stock Meshtastic; one per node plus one at home. |

### Wiring for mesh integration

Three wires per node, always on the XIAO's **D4 (TX) and D5 (RX)** header pins for
the current firmware, so the same carrier PCB fits an S3 or a C5. The GPIO numbers
behind those labels differ per board and are selected at compile time.

| Firmware | Board | XIAO TX → Heltec RX | XIAO RX ← Heltec TX |
|----------|-------|---------------------|---------------------|
| `remoteid-c5-5g` | ESP32-S3 | D4 (GPIO5) | D5 (GPIO6) |
| `remoteid-c5-5g` | ESP32-C5 | D4 (GPIO6) | D5 (GPIO7) |
| `rx5808-detection` | ESP32-S3 | D4 (GPIO5) | D5 (GPIO6) |
| `rx5808-detection` | ESP32-C5 | D4 (GPIO6) | D5 (GPIO7) |
| `node-mode-dualcore` (remote and home) | ESP32-S3 | D4 (GPIO5) | D5 (GPIO6) |
| `remoteid-mesh-dualcore` | ESP32-S3 | D4 (GPIO5) | D5 (GPIO6) |
| `remoteid-mesh` (legacy) | ESP32-S3 | D5 (GPIO6) | D8 (GPIO7) |
| `remoteid-mesh` (legacy) | ESP32-C3 | D4 (GPIO6) | D5 (GPIO7) |

Plus **GND ↔ GND**. On the Heltec side use whichever free GPIOs you configured as
`serial.rxd` / `serial.txd` in Meshtastic (19 / 20 in the reference build). The
`remoteid-c5-5g` build prints its `UART: TX=GPIOx, RX=GPIOy → Heltec` line at boot;
for the others the `SERIAL1_TX_PIN` / `SERIAL1_RX_PIN` constants near the top of the
project's `main.cpp` are the authority.

### Meshtastic node naming

`mesh-mapper.py` learns where a node is from the Meshtastic HTTP API
(`/api/meshtastic_url`) or from manual coordinates (`/api/node_location`). When it
polls Meshtastic it looks for a node whose **shortName or longName matches the
firmware node id** (case-insensitive) and only falls back to the first GPS-equipped
node if nothing matches. Name every Heltec after the node it is wired to.

---

## 🚁 **DJI DroneID Parsing** (`src/dji_droneid.h`)

All four firmware variants (`remoteid-mesh`, `node-mode-dualcore`, `remoteid-mesh-dualcore`, `remoteid-c5-5g`) decode DJI's proprietary Wi-Fi DroneID alongside the existing ASTM/OpenDroneID path.

### How it works

DJI drones broadcast unencrypted telemetry in a vendor-specific 802.11 beacon information element (tag 221 / `0xDD`) under OUI `26:37:12`. This is entirely separate from the ASTM standard — same tag number, different OUI, completely different payload layout.

The parser in `src/dji_droneid.h` is a header-only C implementation. The byte layout is reverse-engineered from Kismet's `dot11_ie_221_dji_droneid.ksy` (credit: Freek van Tienen & Jan Dumon). Key conversions:
- **Lat/lon**: raw `int32` in radians × 10⁷; divided by `174533.0` to yield decimal degrees (= 10⁷ ÷ 57.2958)
- **Yaw**: raw `int16` ÷ 100 ÷ 57.296 → radians

### IE walk integration

Inside the 802.11 beacon frame handler in `main.cpp`, the tag-221 walk checks OUI `26:37:12` *before* the existing OpenDroneID OUIs. A successful parse calls `dji_parse_droneid()` then `dji_emit_json()`, emitting one JSON line per detected frame. The OpenDroneID branch (`FA:0B:BC` / `90:3A:E6`) is unchanged. Both branches share a hardened while loop with explicit bounds guards:

```c
while (offset + 1 < length) {
    int typ = payload[offset];
    int len = payload[offset + 1];
    if (offset + 2 + len > length) break;  /* prevent over-read on malformed IE */
    if (typ == 0xdd && len >= 4) {
        if (/* OUI == 26:37:12 */) { /* DJI path */ }
        else if (/* OUI == FA:0B:BC or 90:3A:E6 */) { /* OpenDroneID path */ }
    }
    offset += len + 2;
}
```

### JSON output

DJI detections are emitted in the same schema as OpenDroneID detections, with two additions (`id_type` and `home_lat`/`home_long`):

```json
{
  "mac": "aa:bb:cc:dd:ee:ff",
  "rssi": -72,
  "id_type": "DJI",
  "basic_id": "1ZNBKXXXXXXXX",
  "drone_lat": 22.319800,
  "drone_long": 114.169500,
  "drone_altitude": 35,
  "height": 28,
  "home_lat": 22.318100,
  "home_long": 114.168900,
  "pilot_lat": 22.318100,
  "pilot_long": 114.168900,
  "product_type": 68
}
```

`pilot_lat`/`pilot_long` are set to the home-point coordinates (the best available operator-position proxy in the IE221 payload; the live operator GPS carried in OpenDroneID's System message has no equivalent in DJI's Wi-Fi IE). `mesh-mapper.py` requires no changes to plot DJI tracks.

### `dji_droneid_t` fields

| Field | Type | Description |
|-------|------|-------------|
| `serial` | `char[17]` | 16-char aircraft serial number |
| `lat`, `lon` | `double` | Aircraft position (degrees) |
| `home_lat`, `home_lon` | `double` | Home/takeoff point (degrees) |
| `altitude` | `int16_t` | Barometric altitude (m) |
| `height` | `int16_t` | Height AGL (m) |
| `v_north`, `v_east`, `v_up` | `int16_t` | Velocity components (raw int16, ~cm/s) |
| `yaw` | `double` | Heading (radians) |
| `product_type` | `uint8_t` | Numeric model ID (DJI internal) |
| `uuid` | `char[21]` | Up to 20-byte UUID string |
| `state_info` | `uint16_t` | Bitfield: bit 0 = serial valid, bit 5 = in air |

---

## 📶 **MAVLink GPS over Wi-Fi** (`src/mavlink_wifi.h`)

All four firmware variants passively extract GPS telemetry from unencrypted ArduPilot/PX4 MAVLink broadcasts using the same promiscuous-mode Wi-Fi interface already used for RemoteID.

### How it works

ArduPilot/PX4 survey and inspection drones often create open (unencrypted) Wi-Fi access points and broadcast MAVLink telemetry on UDP port 14550. The ESP32 captures raw 802.11 data frames in promiscuous mode and extracts GPS from `GLOBAL_POSITION_INT` messages (message ID 33) in both MAVLink v1 and v2 framing. No additional hardware is required.

Rejection is fast:
1. Non-data 802.11 frames → skipped at the first byte
2. WPA/WEP-protected frames (FC byte 1 bit 0x40) → skipped immediately; avoids any attempt to decode encrypted MSDUs
3. Non-IPv4, non-UDP, or non-port-14550 frames → skipped
4. No MAVLink STX byte found → skipped

### JSON output

MAVLink detections emit the same schema as OpenDroneID, with `basic_id` set to `"MAVLink"`:

```json
{
  "mac":            "aa:bb:cc:dd:ee:ff",
  "rssi":           -68,
  "basic_id":       "MAVLink",
  "drone_lat":      22.3198,
  "drone_long":     114.1695,
  "drone_altitude": 35,
  "height":         28,
  "heading":        247
}
```

`pilot_lat`/`pilot_long` are absent — `GLOBAL_POSITION_INT` carries only aircraft position. `mesh-mapper.py` requires no changes to display MAVLink tracks.

### Limitation

The ESP32 listens on one Wi-Fi channel at a time, but the scan schedule sweeps the
whole 2.4 GHz band (plus 5 GHz UNII-3 on the C5), so an aircraft on any channel is
picked up within one sweep. See [Channel scanning](#-channel-scanning) for the
revisit intervals.

---

## 📡 **RX5808 5.8GHz Analog FM Detection** (`rx5808-detection/`)

Detects **analog FPV video transmitters** operating in the 5.645–5.945 GHz band.
Where the other firmware variants look for digital RemoteID broadcasts, this module
uses the RX5808 analog FM receiver IC to sweep all 40 standard FPV channels and
report a signal hit whenever RSSI exceeds your calibrated threshold.

> **Use case:** Spot FPV racing drones or surveillance UAVs that are broadcasting
> analog video but may *not* carry a RemoteID transmitter.

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

### Prerequisites

The VS Code + pioarduino toolchain from [Quick Start Step 1](#step-1--install-the-toolchain-vs-code--pioarduino),
or the PlatformIO CLI. Open the `rx5808-detection/` folder as the project.

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

**Step 5 — Verify on serial monitor**

```bash
pio device monitor --baud 115200
```

You should see within a few seconds:
```json
{"info":"RX5808 scanner ready","node_id":"RX01","channels":40,"threshold":600}
```

If nothing appears for >5 s, check USB-CDC enumeration: the XIAO waits up to 3 s for a host connection before emitting. Replug and reopen the monitor.

**Step 6 — Calibrate threshold (first-time only)**

With no FPV transmitter powered:
1. Watch the monitor for 30 seconds. Any `rssi_raw` values that appear are your **noise floor**.
2. Set `RSSI_THRESHOLD` to (noise floor + 200) in `rx5808.h`. Higher values reduce false positives at the cost of missing weaker signals.
3. Reflash (Step 4). Power a known FPV transmitter nearby and confirm detections.

**Step 7 — Connect to mesh-mapper**

1. Start `mesh-mapper.py` on your server machine.
2. In the web UI Settings panel, select the XIAO's USB serial port (or let it arrive over the mesh via the home node).
3. For range-ring display: set the node's Meshtastic URL or manual coordinates (see Integration section below).

### Configuration

All tuneable constants are at the top of the relevant source files:

| Constant | File | Default | Description |
|----------|------|---------|-------------|
| `RSSI_THRESHOLD` | `src/rx5808.h` | `600` | ADC count above which a signal is reported (RX5808 RSSI spans ~0–1320 counts) |
| `RSSI_SAMPLES` | `src/rx5808.h` | `10` | ADC reads averaged per RSSI measurement |
| `TUNE_SETTLE_MS` | `src/rx5808.h` | `30` | ms to wait for RX5808 PLL after tuning |
| `NODE_ID` | `src/main.cpp` | `"RX01"` | Change per device for multi-node dedup |
| `ENABLE_MESH_RELAY` | `src/main.cpp` | `1` | Set `0` to disable Heltec UART relay |
| `MIN_DWELL_HITS` | `src/main.cpp` | `2` | Both reads must clear threshold to confirm a hit |
| `REPORT_INTERVAL_MS` | `src/main.cpp` | `5000` | Rate-limit re-reports of the same channel (ms) |

### Detection Output

Each hit produces a JSON line on USB Serial (consumed by `mesh-mapper.py`):

```json
{
  "type":     "analog_fm",
  "mac":      "AF:00:16:1A:52:01",
  "freq_mhz": 5658,
  "band":     "R",
  "ch":       1,
  "rssi_raw": 850,
  "rssi":     850,
  "basic_id": "5.8G-R1-5658MHz",
  "node_id":  "RX01"
}
```

The synthetic MAC (`AF:00:…`) encodes frequency + band + channel so that
`mesh-mapper.py` tracks each FPV channel as a distinct "device" — no two
channels share the same identifier.

If `ENABLE_MESH_RELAY` is on, a compact human-readable message is also sent to
the Heltec relay:
```
AnalogFM: R1 5658MHz rssi=850 [RX01]
```

### Integration with mesh-mapper.py

Connect the XIAO via USB, select the port in the mapper UI, and analog FM
detections will begin flowing immediately.

**What appears in the UI:**

| Without node position | With node position |
|---|---|
| Detection listed in no-GPS panel | Dashed-circle range ring on the map |
| Band, channel, frequency, RSSI shown | 📡 marker at the node's GPS location |
| No map marker | Ring radius = FSPL-derived max detection range |

To get the range ring, give mesh-mapper a position for the node. Two options:

**Option A — Meshtastic HTTP API (automatic, updates every 30 s):**
Open the mapper Settings panel and enter the Heltec node's URL:
```
POST /api/meshtastic_url  { "node_id": "RX01", "url": "http://192.168.1.x" }
```
The node's `shortName` or `longName` in Meshtastic must match `NODE_ID` in the firmware (e.g. `"RX01"`). The poller will pick up the correct node even in a multi-node mesh.

**Option B — manual coordinates:**
```
POST /api/node_location  { "node_id": "RX01", "lat": 25.7617, "lon": -80.1918 }
```

Differences from RemoteID detections:
- `type: "analog_fm"` is logged at INFO level with band/channel/RSSI.
- Range rings are colored by signal strength: green ≥ 1000, amber ≥ 800, red below (within the RX5808's ~0–1320 ADC-count range).
- Each detection is also forwarded to ATAK/WinTAK as two CoT events: a `a-u-G-E-S` sensor marker and a `u-r-b-c-c` range ring shape.

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

## 🛠️ **API Reference**

### **Core Endpoints**

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/` | Main web interface |
| `GET` | `/api/detections` | Current active drone detections |
| `POST` | `/api/detections` | Submit new detection data |
| `GET` | `/api/detections_history` | Historical detection data (GeoJSON) |
| `GET` | `/api/paths` | Flight path data for visualization |
| `POST` | `/api/reactivate/<mac>` | Reactivate inactive drone detection |

### **Device Management**

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/aliases` | Get device aliases |
| `POST` | `/api/set_alias` | Set friendly name for device |
| `POST` | `/api/clear_alias/<mac>` | Remove device alias |
| `GET` | `/api/ports` | Available serial ports |
| `GET` | `/api/serial_status` | ESP32 connection status |
| `GET` | `/api/selected_ports` | Currently configured ports |

### **External Integration**

| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | `/api/set_webhook_url` | Configure webhook endpoint |
| `GET` | `/api/get_webhook_url` | Get current webhook URL |
| `POST` | `/api/webhook_popup` | Webhook notification handler |

### **Data Export**

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/download/csv` | Download current detections (CSV) |
| `GET` | `/download/kml` | Download current detections (KML) |
| `GET` | `/download/aliases` | Download device aliases |
| `GET` | `/download/cumulative_detections.csv` | Download full history (CSV) |
| `GET` | `/download/cumulative.kml` | Download full history (KML) |

### **RX5808 / TAK Integration**

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET/POST` | `/api/node_location` | Get or set manual GPS for an RX5808 node |
| `GET/POST` | `/api/meshtastic_url` | Get or set Meshtastic HTTP API URL for a node |
| `GET` | `/api/tak_contacts` | Current inbound ATAK/WinTAK/iTAK operator positions |

### **System Management**

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/diagnostics` | System health and performance |
| `POST` | `/api/debug_mode` | Toggle debug logging |
| `POST` | `/api/send_command` | Send command to ESP32 devices |
| `GET` | `/select_ports` | Port selection interface |
| `POST` | `/select_ports` | Update port configuration |

### **WebSocket Events**

Real-time events pushed to connected clients:

- `detections` - Full drone detection state (all tracked MACs)
- `detection` - Single detection update (real-time, per-event)
- `paths` - Updated flight path data
- `serial_status` - ESP32 connection status changes
- `aliases` - Device alias updates
- `cumulative_log` - Historical data updates
- `tak_contact` - Inbound ATAK/WinTAK/iTAK operator position (lat/lon/callsign/type)

---

## 🐛 **Troubleshooting**

**Build fails or the C5 env is missing**
- Make sure the *pioarduino IDE* extension (or a recent PlatformIO CLI) is in use and
  the project folder, not the repo root, is open. The platform is pinned by URL in
  `platformio.ini`; delete the project's `.pio/` folder to force a clean re-download.

**Upload cannot connect**
- On the ESP32-C5: hold **BOOT**, tap **RESET**, release **BOOT**, retry.
- On Linux check the udev rules and `dialout` membership from Quick Start Step 1, then
  `ls /dev/ttyACM* /dev/ttyUSB*`.
- With several boards attached set `upload_port` in `platformio.ini`.

**Node boots but scans few channels**
- `channel N rejected by regulatory domain` on the monitor means
  `WIFI_CHAN_START`/`WIFI_CHAN_COUNT` exclude a channel in the scan list. Fix the
  defines and rebuild. See [Regulatory domain](#regulatory-domain).

**Detections reach the mapper but the RX5808 range ring is missing or misplaced**
- The Meshtastic node's shortName/longName must equal the firmware `NODE_ID`; otherwise
  the mapper falls back to the first GPS node in the mesh. Or set coordinates by hand via
  `/api/node_location`.

**No serial data at the host**
- `mesh-mapper.py --debug` and watch `mapper.log`. Confirm the home node's port is
  selected in Settings (`/api/serial_status`). Confirm the Heltec serial module is in
  `TEXTMSG` mode at 115200 on the wired pins, and that TX/RX are crossed.

**Web interface not loading**
```bash
ss -tlnp | grep :5000      # is the server listening?
tail -f mapper.log
```

**No drone detections at all**
- Verify the node is flashed with a detection build, not `home_node`, and prints its
  startup banner on the serial monitor at 115200.
- Confirm the aircraft actually broadcasts something in the
  [protocol coverage](#protocol-coverage) table; OcuSync-only DJI models are invisible
  to an ESP32.
- Run `mapper_test/mapper_test.py` to prove the mapper side end-to-end.

---

## 📄 **License**

This project is licensed under the MIT License, as is the upstream project it is
derived from.

---

## 🙏 **Acknowledgments**

- **ColonelPanic** and **Luke Switzer** — the original
  [drone-mesh-mapper](https://github.com/colonelpanichacks/drone-mesh-mapper)
- **Cemaxacutor**
- **"Alik",** 93rd OMBr, Armed Forces of Ukraine
- **"Ivan",** 427th Rarog, Armed Forces of Ukraine
- **OpenDroneID Community** — standards and reference implementation
- **Kismet** (Freek van Tienen & Jan Dumon) — DJI DroneID IE layout
- **pioarduino** — the maintained Arduino-ESP32 platform for PlatformIO that makes the C5 build possible
- Thank you PCBway for the awesome boards! The combination of their top tier quality, competitive pricing, fast turnaround times, and stellar customer service makes PCBWay the go-to choice for professional PCB fabrication, whether you're prototyping innovative mesh detection systems or scaling up for full production runs.
https://www.pcbway.com/
  <div align="center"> <img src="boards.png" alt="boards" style="width:50%; height:25%;">


---

## 🛒 **Hardware Store**

Get professional PCBs from ColonelPanic's store:
https://colonelpanic.tech

Made with ❤️ by the Drone Detection Community

</div>
