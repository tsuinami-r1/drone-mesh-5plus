# <div align="center">  **Drone Remote ID Mapper** </div>

<div align="center">

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Python](https://img.shields.io/badge/Python-3.7+-blue.svg)](https://www.python.org/)
[![ESP32](https://img.shields.io/badge/ESP32-Compatible-green.svg)](https://www.espressif.com/)
[![Flask](https://img.shields.io/badge/Flask-2.0+-red.svg)](https://flask.palletsprojects.com/)

**Real-time drone detection, mapping, and Remote ID compliance monitoring**

[🚀 Quick Start](#-quick-start) • [📋 Features](#-features) • [🛠️ API Reference](#-api-reference) • [🔧 Hardware](#-hardware-setup)

<img src="eye.png" alt="Drone Detection Eye" style="width:50%; height:25%;">

</div>

---

RX5808

GPIO9  (D10) → RX5808 DATA

GPIO7  (D8)  → RX5808 CLK

GPIO8  (D9)  → RX5808 CS

GPIO1  (D0)  ← RX5808 RSSI  (analog)

3.3V         → RX5808 VCC

GND          → RX5808 GND


GPIO5 / GPIO6 still available for Heltec LoRa relay (ENABLE_MESH_RELAY 1)



## 🛠️ **Hardware Options**

### **🎯 Ready-to-Use Solution**
Pre-built detection hardware designed specifically for this project:

<a href="https://www.tindie.com/stores/colonel_panic/?ref=offsite_badges&utm_source=sellers_colonel_panic&utm_medium=badges&utm_campaign=badge_large">
    <img src="https://d2ss6ovg47m0r5.cloudfront.net/badges/tindie-larges.png" alt="I sell on Tindie" width="200" height="104">
</a>

**✅ Complete kits with all components included**  
**✅ Pre-flashed firmware ready to use**  


**🔋 Completely Standalone Operation**
- **No Raspberry Pi Required**: Boards operate independently for mesh detection
- **No Computer Needed**: Self-contained drone detection and mesh communication
- **Instant Setup**: Just power on and start detecting

**📊 Optional Mapper Integration**
- **Standalone mesh detection** works great on its own
- **Add the mapper software** for enhanced visualization and logging
- **Best of both worlds**: Mesh detection + centralized monitoring

### **🔧 DIY Build Option**

Build your own detection system using readily available components:

**Required Components:**
- **Xiao ESP32-S3** (dual-core with WiFi + Bluetooth)
- **Heltec WiFi LoRa 32 V3** (for mesh networking)
- Basic wiring connections

**Perfect for:**
- Learning and experimentation
- Custom modifications
- Budget-conscious builds
- Educational projects

---

## 🎯 **Overview**

Advanced drone detection system that captures and maps Remote ID broadcasts from drones using ESP32 hardware. Features real-time web interface, persistent tracking across sessions, and comprehensive data export capabilities.

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
- ✅ **Core Dependencies**: Flask, Flask-SocketIO, pyserial, requests, urllib3
- ✅ **Optional Packages**: Performance and development tools

### 📖 **Manual Setup**

1. **Download mapper**
   ```bash
   wget https://raw.githubusercontent.com/colonelpanichacks/drone-mesh-mapper/main/mesh-mapper.py
   ```

2. **Install dependencies**
   ```bash
   pip3 install Flask Flask-SocketIO pyserial requests urllib3 python-socketio eventlet
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
- **FAA Integration**: Automatic Remote ID registration lookups
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

### **FAA & External Integration**

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/faa/<identifier>` | FAA registration lookup |
| `POST` | `/api/query_faa` | Manual FAA query |
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
- `faa_cache` - FAA lookup results

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
