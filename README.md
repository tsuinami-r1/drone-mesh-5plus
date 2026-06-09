# <div align="center">  **Drone Remote ID Mapper** </div>

<div align="center">

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Python](https://img.shields.io/badge/Python-3.7+-blue.svg)](https://www.python.org/)
[![ESP32](https://img.shields.io/badge/ESP32-Compatible-green.svg)](https://www.espressif.com/)
[![Flask](https://img.shields.io/badge/Flask-2.0+-red.svg)](https://flask.palletsprojects.com/)

**Real-time drone detection, mapping, and Remote ID monitoring — ASTM OpenDroneID + DJI DroneID**

[🚀 Quick Start](#-quick-start) • [📋 Features](#-features) • [🛠️ API Reference](#-api-reference) • [🔧 Hardware](#-hardware-setup)

<img src="eye.png" alt="Drone Detection Eye" style="width:50%; height:25%;">

</div>

---

## 🎯 **Overview**

Advanced drone detection system that captures and maps Remote ID broadcasts from drones using ESP32 hardware. Features real-time web interface, persistent tracking across sessions, and comprehensive data export capabilities.

Original code by Luke Switzer and ColonelPanic. Currently prototyping RX5808 integration for analog 5.8 GHz FM scanning.

### Protocol coverage

| Protocol | Transport | OUI / identifier | Firmware support |
|----------|-----------|-----------------|-----------------|
| ASTM F3411 / OpenDroneID | 802.11 beacon (IE 221) | `FA:0B:BC` | All variants |
| ASD-STAN 4709-002 | 802.11 beacon (IE 221) | `90:3A:E6` | All variants |
| ASTM F3411 / OpenDroneID | Wi-Fi NAN action frame | `org.opendroneid.remoteid` hash | All variants |
| **DJI DroneID** | **802.11 beacon (IE 221)** | **`26:37:12`** | **All variants (this branch)** |
| DJI OcuSync / O3 / O4 | OFDM (~2.4295 GHz video band) | — | ❌ Requires SDR |

> **DJI Wi-Fi coverage caveat:** Modern DJI aircraft (Mini 3 Pro, Air 3, Mavic 3 series) primarily use OcuSync/O3/O4 for their DroneID downlink, which is OFDM in the video band and **cannot be demodulated by an ESP32**. The Wi-Fi IE221 DroneID broadcast (`26:37:12`) is present on older and budget models; treat it as partial fleet coverage, not "all DJI".

---

## ⚡ **Quick Start**

### 🔧 **Automated Setup** (Recommended)

Download and install everything automatically using the official RPI setup scripts:

```bash
# Download the RPI setup script
wget https://raw.githubusercontent.com/colonelpanichacks/drone-mesh-mapper/main/RPI/install_rpi.py

# Install from main branch (stable)
python3 install_rpi.py --branch main

# Or install from Dev branch (latest features)  
python3 install_rpi.py --branch Dev
```

**Advanced Setup Options:**
```bash
# Custom installation directory
python3 install_rpi.py --branch main --install-dir /opt/mesh-mapper

# Skip auto-start cron job
python3 install_rpi.py --branch main --no-cron

# Force overwrite existing installation
python3 install_rpi.py --branch Dev --force
```

### 📦 **Dependency Installation**

Install all required dependencies automatically:

```bash
# Download and run the universal dependency installer
wget https://raw.githubusercontent.com/colonelpanichacks/drone-mesh-mapper/main/RPI/rpi_dependancies.py
python3 rpi_dependancies.py
```

This installer handles:
- ✅ **System Detection**: Automatically detects Linux, macOS, Windows
- ✅ **Package Manager Support**: apt, yum, dnf, pacman, brew, pkg
- ✅ **Python & pip**: Ensures compatible Python 3.7+ and pip installation
- ✅ **Core Dependencies**: Flask, Flask-SocketIO, pyserial, requests
- ✅ **Optional Packages**: Performance and development tools

### 📖 **Manual Setup**

1. **Download mapper**
   ```bash
   wget https://raw.githubusercontent.com/colonelpanichacks/drone-mesh-mapper/main/mesh-mapper.py
   ```

2. **Install dependencies**
   ```bash
   pip3 install Flask Flask-SocketIO pyserial requests python-socketio eventlet
   ```

3. **Flash ESP32 firmware**
   - Choose appropriate firmware from `firmware/` directory
   - Use Arduino IDE, PlatformIO, or esptool.py
   - Configure WiFi channel and mesh settings

4. **Run Mapper**
   ```bash
   python3 mesh-mapper.py
   ```

---

## 📋 **Core Features**

### 🗺️ **Real-time Mapping**
- **Live Detection Display**: Interactive map showing drone positions as they're detected
- **Flight Path Tracking**: Visual trails showing drone and pilot movement over time
- **Persistent Sessions**: Drones remain visible across application restarts
- **Multi-device Support**: Handle multiple ESP32 receivers simultaneously

### 📊 **Data Management**
- **Detection History**: Complete log of all drone encounters with timestamps
- **Device Aliases**: Assign friendly names to frequently seen drones
- **Export Formats**: Download data as CSV, KML (Google Earth), or GeoJSON
- **Cumulative Logging**: Long-term historical data storage

### 🔧 **ESP32 Integration**
- **Auto-detection**: Automatically finds and connects to ESP32 devices
- **Port Management**: Save and restore USB port configurations
- **Status Monitoring**: Real-time connection health and data flow indicators
- **Command Interface**: Send diagnostic commands to connected hardware

### 🌐 **Web Interface**
- **Real-time Updates**: WebSocket-powered live data streaming
- **Mobile Responsive**: Works on desktop, tablet, and mobile devices
- **Multiple Views**: Map, detection list, and device status panels
- **Data Export**: Download detections directly from web interface

### ⚙️ **Configuration & Monitoring**
- **Headless Operation**: Run without web interface for dedicated deployments
- **Debug Logging**: Detailed logging for troubleshooting and development
- **Webhook Support**: External system integration via HTTP callbacks

---

## 🚀 **Usage**

### **Command Line Options**

```bash
python3 mesh-mapper.py [OPTIONS]
```

| Option | Description | Default |
|--------|-------------|---------|
| `--headless` | Run without web interface | false |
| `--debug` | Enable debug logging | false |
| `--web-port PORT` | Web interface port | 5000 |
| `--port-interval SECONDS` | Port monitoring interval | 10 |
| `--no-auto-start` | Disable automatic port connection | false |

### **Examples**

```bash
# Standard operation with web interface
python3 mesh-mapper.py

# Headless operation for dedicated server
python3 mesh-mapper.py --headless --debug

# Custom web port with verbose logging
python3 mesh-mapper.py --web-port 8080 --debug

# Disable auto-connection to saved ports
python3 mesh-mapper.py --no-auto-start
```

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

- `detections` - Updated drone detection data
- `paths` - Updated flight path data  
- `serial_status` - ESP32 connection status changes
- `aliases` - Device alias updates
- `cumulative_log` - Historical data updates

---

## 🔧 **Hardware Setup**

### **Supported ESP32 Boards**
- ✅ **Xiao ESP32-C3** (Single core, WiFi only)
- ✅ **Xiao ESP32-S3** (Dual core, WiFi + Bluetooth)  
- ✅ **ESP32-DevKit** (Development and testing)
- ✅ **Custom PCBs** (See Tindie store link below)

### **Wiring for Mesh Integration**
```
ESP32 Pin | Mesh Radio Pin
----------|---------------
TX1 (d4)  | RX 19
RX1 (d5)  | TX 20
3.3V      | VCC
GND       | GND
```

---

## 🚁 **DJI DroneID Parsing** (`src/dji_droneid.h`)

All four firmware variants (`remoteid-mesh`, `node-mode-dualcore`, `remoteid-mesh-dualcore`, `remoteid-c5-5g`) now decode DJI's proprietary Wi-Fi DroneID alongside the existing ASTM/OpenDroneID path.

### How it works

DJI drones broadcast unencrypted telemetry in a vendor-specific 802.11 beacon information element (tag 221 / `0xDD`) under OUI `26:37:12`. This is entirely separate from the ASTM standard — same tag number, different OUI, completely different payload layout.

The parser in `src/dji_droneid.h` is a header-only C implementation. The byte layout is reverse-engineered from Kismet's `dot11_ie_221_dji_droneid.ksy` (credit: Freek van Tienen & Jan Dumon). Key conversions:
- **Lat/lon**: raw `int32` in radians × 10⁷; divided by `174533.0` to yield decimal degrees (= 10⁷ ÷ 57.2958)
- **Yaw**: raw `int16` ÷ 100 ÷ 57.296 → radians

### IE walk integration

Inside the 802.11 beacon frame handler in `main.cpp`, the tag-221 walk now checks OUI `26:37:12` *before* the existing OpenDroneID OUIs. A successful parse calls `dji_parse_droneid()` then `dji_emit_json()`, emitting one JSON line per detected frame. The OpenDroneID branch (`FA:0B:BC` / `90:3A:E6`) is unchanged. Both branches share a hardened while loop with explicit bounds guards:

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
| **Seeed XIAO ESP32-S3** | Same board used by other variants |
| **RX5808 module** | 5.8GHz analog FM receiver; ~$5–10, widely available |
| Jumper wires | 4 signal + 2 power wires |

### Wiring — XIAO ESP32-S3 ↔ RX5808

```
XIAO ESP32-S3 Pin        RX5808 Pin
──────────────────────── ──────────
GPIO9  (D10 / MOSI)  →   DATA
GPIO7  (D8  / SCK)   →   CLK
GPIO8  (D9)          →   CS       (active LOW)
GPIO1  (D0  / ADC)   ←   RSSI     (analog output)
3.3V                 →   VCC
GND                  →   GND
```

Optional — connect a **Heltec LoRa V3** for mesh relay (same wiring as all other
firmware variants):

```
GPIO5 (D4) → Heltec RX
GPIO6 (D5) ← Heltec TX
```

### Build & Flash

```bash
cd rx5808-detection

# Compile
pio run -e seeed_xiao_esp32s3

# Compile and upload (XIAO connected via USB)
pio run -e seeed_xiao_esp32s3 --target upload

# Open serial monitor to verify
pio device monitor --baud 115200
```

On power-up you should see:
```json
{"info":"RX5808 scanner ready","node_id":"RX01","channels":40,"threshold":1800}
```

### Configuration

All tuneable constants are at the top of the relevant source files:

| Constant | File | Default | Description |
|----------|------|---------|-------------|
| `RSSI_THRESHOLD` | `src/rx5808.h` | `1800` | ADC count (0–4095) above which a signal is reported |
| `RSSI_SAMPLES` | `src/rx5808.h` | `10` | ADC reads averaged per measurement |
| `TUNE_SETTLE_MS` | `src/rx5808.h` | `30` | ms to wait for RX5808 PLL after tuning |
| `NODE_ID` | `src/main.cpp` | `"RX01"` | Change per device for multi-node dedup |
| `ENABLE_MESH_RELAY` | `src/main.cpp` | `1` | Set `0` to disable Heltec UART relay |
| `MIN_DWELL_HITS` | `src/main.cpp` | `2` | Consecutive reads required to confirm detection |
| `REPORT_INTERVAL_MS` | `src/main.cpp` | `5000` | Rate-limit re-reports of the same channel |

### Calibrating the RSSI Threshold

1. Flash the firmware and open the serial monitor.
2. With **no FPV transmitter powered on**, let it scan for ~30 seconds and note
   the `rssi_raw` values in any detections.  That is your **noise floor**.
3. Set `RSSI_THRESHOLD` to **noise floor + 200** (at minimum).  Higher values
   reduce false positives at the cost of missing weaker signals.
4. Re-flash, power up a known FPV transmitter nearby, and confirm detections appear.

### Detection Output

Each hit produces a JSON line on USB Serial (consumed by `mesh-mapper.py`):

```json
{
  "type":     "analog_fm",
  "mac":      "AF:00:16:1A:52:01",
  "freq_mhz": 5658,
  "band":     "R",
  "ch":       1,
  "rssi_raw": 2240,
  "rssi":     2240,
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
AnalogFM: R1 5658MHz rssi=2240 [RX01]
```

### Integration with mesh-mapper.py

No extra configuration is needed. Connect the XIAO via USB, select the port in
the mapper UI, and analog FM detections will appear in the **no-GPS detections
panel** alongside RemoteID detections.

Differences from RemoteID detections:
- `type: "analog_fm"` is logged prominently at INFO level with band/channel/RSSI.
- No map marker is placed (there are no GPS coordinates from an RX5808 alone).

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

## 📊 **Performance**

| Metric | Performance |
|--------|-------------|
| **Detection Latency** | < 500ms average |
| **Concurrent Drones** | 50+ simultaneous |
| **Memory Usage** | < 100MB typical |
| **Storage Efficiency** | ~1KB per detection |
| **Network Throughput** | 1000+ detections/min |

---

## 🐛 **Troubleshooting**

### **Common Issues**

**ESP32 Not Detected**
```bash
# Check USB connection
ls -la /dev/tty* | grep USB

# Verify driver installation  
dmesg | grep tty
```

**Web Interface Not Loading**
```bash
# Check if service is running
netstat -tlnp | grep :5000

# Review logs
tail -f mesh-mapper.log
```

**No Drone Detections**
- Verify ESP32 firmware is properly flashed
- Check WiFi channel configuration (default: channel 6)
- Ensure drones are transmitting Remote ID (required in many jurisdictions)

---

## 📄 **License**

This project is licensed under the MIT License 

---

## 🙏 **Acknowledgments**

- **Cemaxacutor** 
- **Luke Switzer** 
- **OpenDroneID Community** - Standards and specifications
- Thank you PCBway for the awesome boards! The combination of their top tier quality, competitive pricing, fast turnaround times, and stellar customer service makes PCBWay the go-to choice for professional PCB fabrication, whether you're prototyping innovative mesh detection systems or scaling up for full production runs.
https://www.pcbway.com/
  <div align="center"> <img src="boards.png" alt="boards" style="width:50%; height:25%;">


---

## 🛒 **Hardware Store**

Get professional PCBs and complete kits:

<a href="https://www.tindie.com/stores/colonel_panic/?ref=offsite_badges&utm_source=sellers_colonel_panic&utm_medium=badges&utm_campaign=badge_large">
    <img src="https://d2ss6ovg47m0r5.cloudfront.net/badges/tindie-larges.png" alt="I sell on Tindie" width="200" height="104">
</a>

---

<div align="center">

**⭐ If this project helped you, please give it a star! ⭐**

Made with ❤️ by the Drone Detection Community

</div> 
