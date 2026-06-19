/*
 * RemoteID Mesh Detect — Dual-Band Edition
 *
 * Supports ESP32-C5 (dual-band 2.4GHz + 5GHz WiFi 6) and ESP32-S3 (2.4GHz only)
 * Detects drones broadcasting RemoteID via WiFi (NAN/Beacon) and Bluetooth LE
 *
 * Output:
 *   USB Serial  — JSON lines for mesh-mapper.py
 *   Serial1 UART (TX=GPIO5, RX=GPIO6) — compact messages for Heltec/Meshtastic relay
 *
 * For ESP32-C5: Dual-band scanning with fast channel hopping across 2.4+5GHz
 * For ESP32-S3: Single-band 2.4GHz scanning (original behavior)
 */

#if !defined(ARDUINO_ARCH_ESP32)
  #error "This program requires an ESP32"
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include "opendroneid.h"
#include "odid_wifi.h"
#include "dji_droneid.h"
#include "bt_odid.h"
#include "mavlink_wifi.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// UART Pins — same wiring as remoteid-mesh-dualcore (Heltec LoRa V3)
// ============================================================================

const int SERIAL1_TX_PIN = 5;   // GPIO5 → Heltec RX
const int SERIAL1_RX_PIN = 6;   // GPIO6 → Heltec TX

// ============================================================================
// Board-specific configuration
// ============================================================================

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(ARDUINO_XIAO_ESP32C5)
  #define BOARD_IS_C5 1
  #define DUAL_BAND_ENABLED true
  #define BOARD_NAME "XIAO ESP32-C5 (Dual-Band)"
  // C5 is single-core RISC-V
  #define SINGLE_CORE 1
#else
  #define BOARD_IS_C5 0
  #define DUAL_BAND_ENABLED false
  #define BOARD_NAME "XIAO ESP32-S3 (2.4GHz)"
  #define SINGLE_CORE 0
#endif

// ============================================================================
// Dual-Band Channel Configuration
// ============================================================================

// All channels to scan: 2.4 GHz primary (1/6/11) + 5 GHz UNII-3 (149–165)
// Dwell 50 ms each → full cycle ≈ 400 ms, revisiting each 2.4 GHz channel ~2.5×/s
static const uint8_t all_channels[] = {1, 6, 11, 149, 153, 157, 161, 165};
#define NUM_ALL_CHANNELS (sizeof(all_channels) / sizeof(all_channels[0]))

// Kept for reference by Serial printf in channelHopTask
#define DWELL_TIME_MS 50

// ============================================================================
// WiFi Band Enum
// ============================================================================

enum WiFiBand {
  BAND_UNKNOWN = 0,
  BAND_2_4GHZ  = 1,
  BAND_5GHZ    = 2,
  BAND_BLE     = 3
};

// ============================================================================
// Data Structures
// ============================================================================

struct id_data {
  uint8_t  mac[6];
  int      rssi;
  uint32_t last_seen;
  char     op_id[ODID_ID_SIZE + 1];
  char     uav_id[ODID_ID_SIZE + 1];
  double   lat_d;
  double   long_d;
  double   base_lat_d;
  double   base_long_d;
  int      altitude_msl;
  int      height_agl;
  int      speed;
  int      heading;
  int      flag;
  WiFiBand band;
  uint8_t  channel;
};

// ============================================================================
// Function Prototypes
// ============================================================================

void callback(void *, wifi_promiscuous_pkt_type_t);
void send_json_fast(const id_data *UAV);
void print_compact_message(const id_data *UAV);

// ============================================================================
// Global Variables
// ============================================================================

#define MAX_UAVS 8
id_data uavs[MAX_UAVS] = {0};
NimBLEScan* pBLEScan = nullptr;
ODID_UAS_Data UAS_data;
unsigned long last_status = 0;

// Current channel tracking (for dual-band)
volatile uint8_t current_channel = 6;  /* channelHopTask updates this */
volatile WiFiBand current_band = BAND_2_4GHZ;
static portMUX_TYPE channelMux = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t printQueue;

// ============================================================================
// UAV Tracking
// ============================================================================

id_data* next_uav(const uint8_t* mac) {
  for (int i = 0; i < MAX_UAVS; i++) {
    if (memcmp(uavs[i].mac, mac, 6) == 0)
      return &uavs[i];
  }
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] == 0)
      return &uavs[i];
  }
  // Evict oldest entry
  uint32_t oldest_time = UINT32_MAX;
  int oldest_idx = 0;
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].last_seen < oldest_time) {
      oldest_time = uavs[i].last_seen;
      oldest_idx = i;
    }
  }
  return &uavs[oldest_idx];
}

// ============================================================================
// BLE Scanning Callbacks (NimBLE 2.1.0)
// ============================================================================

class MyAdvertisedDeviceCallbacks : public NimBLEScanCallbacks {
public:
  void onResult(const NimBLEAdvertisedDevice* device) override {
    const std::vector<uint8_t>& payloadVec = device->getPayload();
    int adv_len = (int)payloadVec.size();
    if (adv_len <= 0) return;
    const uint8_t *adv = payloadVec.data();

    const uint8_t *msgs;
    int msgs_len;
    if (!bt_odid_find_odid(adv, adv_len, &msgs, &msgs_len) || msgs_len < 1) return;

    const uint8_t *mac = device->getAddress().getBase()->val;
    id_data *UAV = next_uav(mac);
    UAV->last_seen = millis();
    UAV->rssi = device->getRSSI();
    memcpy(UAV->mac, mac, 6);
    UAV->band = BAND_BLE;
    UAV->channel = 0;

    const uint8_t *odid = msgs;
    switch (odid[0] & 0xF0) {
      case 0x00: {
        ODID_BasicID_data basic;
        decodeBasicIDMessage(&basic, (ODID_BasicID_encoded *)odid);
        strncpy(UAV->uav_id, (char *)basic.UASID, ODID_ID_SIZE);
        UAV->uav_id[ODID_ID_SIZE] = '\0';
        break;
      }
      case 0x10: {
        ODID_Location_data loc;
        decodeLocationMessage(&loc, (ODID_Location_encoded *)odid);
        UAV->lat_d = loc.Latitude;
        UAV->long_d = loc.Longitude;
        UAV->altitude_msl = (int)loc.AltitudeGeo;
        UAV->height_agl = (int)loc.Height;
        UAV->speed = (int)loc.SpeedHorizontal;
        UAV->heading = (int)loc.Direction;
        break;
      }
      case 0x40: {
        ODID_System_data sys;
        decodeSystemMessage(&sys, (ODID_System_encoded *)odid);
        UAV->base_lat_d = sys.OperatorLatitude;
        UAV->base_long_d = sys.OperatorLongitude;
        break;
      }
      case 0x50: {
        ODID_OperatorID_data op;
        decodeOperatorIDMessage(&op, (ODID_OperatorID_encoded *)odid);
        strncpy(UAV->op_id, (char *)op.OperatorId, ODID_ID_SIZE);
        break;
      }
    }
    UAV->flag = 1;
    {
      id_data tmp = *UAV;
      xQueueSend(printQueue, &tmp, 0);
    }
  }
};

// ============================================================================
// JSON Output (USB Serial → mesh-mapper.py)
// ============================================================================

const char* bandToString(WiFiBand band) {
  switch (band) {
    case BAND_2_4GHZ: return "2.4GHz";
    case BAND_5GHZ:   return "5GHz";
    case BAND_BLE:    return "BLE";
    default:          return "unknown";
  }
}

void send_json_fast(const id_data *UAV) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);
  char json_msg[320];
  snprintf(json_msg, sizeof(json_msg),
    "{\"mac\":\"%s\",\"rssi\":%d,\"band\":\"%s\",\"channel\":%d,"
    "\"drone_lat\":%.6f,\"drone_long\":%.6f,\"drone_altitude\":%d,"
    "\"pilot_lat\":%.6f,\"pilot_long\":%.6f,\"basic_id\":\"%s\"}",
    mac_str, UAV->rssi, bandToString(UAV->band), UAV->channel,
    UAV->lat_d, UAV->long_d, UAV->altitude_msl,
    UAV->base_lat_d, UAV->base_long_d, UAV->uav_id);
  Serial.println(json_msg);
}

// ============================================================================
// Compact Message Output (Serial1 UART → Heltec/Meshtastic)
// ============================================================================

void print_compact_message(const id_data *UAV) {
  static unsigned long lastSendTime = 0;
  const unsigned long sendInterval = 5000;
  const int MAX_MESH_SIZE = 230;

  if (millis() - lastSendTime < sendInterval) return;
  lastSendTime = millis();

  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);

  char mesh_msg[MAX_MESH_SIZE];
  int msg_len = 0;
  msg_len += snprintf(mesh_msg + msg_len, sizeof(mesh_msg) - msg_len,
                      "Drone[%s]: %s RSSI:%d",
                      bandToString(UAV->band), mac_str, UAV->rssi);
  if (msg_len < MAX_MESH_SIZE && UAV->lat_d != 0.0 && UAV->long_d != 0.0) {
    msg_len += snprintf(mesh_msg + msg_len, sizeof(mesh_msg) - msg_len,
                        " https://maps.google.com/?q=%.6f,%.6f",
                        UAV->lat_d, UAV->long_d);
  }
  if (Serial1.availableForWrite() >= msg_len) {
    Serial1.println(mesh_msg);
  }

  if (UAV->base_lat_d != 0.0 && UAV->base_long_d != 0.0) {
    char pilot_msg[MAX_MESH_SIZE];
    int pilot_len = snprintf(pilot_msg, sizeof(pilot_msg),
                             "Pilot: https://maps.google.com/?q=%.6f,%.6f",
                             UAV->base_lat_d, UAV->base_long_d);
    if (Serial1.availableForWrite() >= pilot_len) {
      Serial1.println(pilot_msg);
    }
  }
}

// ============================================================================
// Channel Hopping Task (C5 dual-band only)
// ============================================================================

#if DUAL_BAND_ENABLED
void channelHopTask(void *parameter) {
  Serial.println("[DUAL-BAND] Channel hopping active");
  Serial.print("[DUAL-BAND] Channels: ");
  for (int i = 0; i < (int)NUM_ALL_CHANNELS; i++) {
    Serial.printf("%d%s", all_channels[i], (i < (int)NUM_ALL_CHANNELS - 1) ? "," : "\n");
  }

  uint8_t idx = 0;
  for (;;) {
    uint8_t ch = all_channels[idx];
    WiFiBand band = (ch > 20) ? BAND_5GHZ : BAND_2_4GHZ;

    portENTER_CRITICAL(&channelMux);
    current_channel = ch;
    current_band = band;
    portEXIT_CRITICAL(&channelMux);

    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    idx = (idx + 1) % NUM_ALL_CHANNELS;
    vTaskDelay(pdMS_TO_TICKS(DWELL_TIME_MS));
  }
}
#endif

// ============================================================================
// BLE Scan Task
// ============================================================================

void bleScanTask(void *parameter) {
  for (;;) {
    pBLEScan->getResults(1000, false);
    pBLEScan->clearResults();
    delay(100);
  }
}

// ============================================================================
// WiFi Promiscuous Mode Callback
// ============================================================================

static void processODIDData(id_data* UAV) {
  if (UAS_data.BasicIDValid[0])
    strncpy(UAV->uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
  UAV->uav_id[ODID_ID_SIZE] = '\0';
  if (UAS_data.LocationValid) {
    UAV->lat_d = UAS_data.Location.Latitude;
    UAV->long_d = UAS_data.Location.Longitude;
    UAV->altitude_msl = (int)UAS_data.Location.AltitudeGeo;
    UAV->height_agl = (int)UAS_data.Location.Height;
    UAV->speed = (int)UAS_data.Location.SpeedHorizontal;
    UAV->heading = (int)UAS_data.Location.Direction;
  }
  if (UAS_data.SystemValid) {
    UAV->base_lat_d = UAS_data.System.OperatorLatitude;
    UAV->base_long_d = UAS_data.System.OperatorLongitude;
  }
  if (UAS_data.OperatorIDValid)
    strncpy(UAV->op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
}

static void storeAndQueue(id_data* UAV) {
  id_data* storedUAV = next_uav(UAV->mac);
  *storedUAV = *UAV;
  storedUAV->flag = 1;
  {
    id_data tmp = *storedUAV;
    xQueueSend(printQueue, &tmp, 0);
  }
}

void callback(void *buffer, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

  wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buffer;
  uint8_t *payload = packet->payload;
  int length = packet->rx_ctrl.sig_len;

  // Get current channel/band info (thread-safe)
  uint8_t detect_channel;
  WiFiBand detect_band;
  portENTER_CRITICAL(&channelMux);
  detect_channel = current_channel;
  detect_band = current_band;
  portEXIT_CRITICAL(&channelMux);

  if (length < 16) return;   /* too short to safely read addr/NAN fields */

  if (type == WIFI_PKT_DATA) {
    uint8_t mav_mac[6];
    mav_gps_t mav_gps;
    if (mav_wifi_extract(payload, length, mav_mac, &mav_gps)) {
      id_data UAV = {};
      memcpy(UAV.mac, mav_mac, 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();
      UAV.lat_d = mav_gps.lat;
      UAV.long_d = mav_gps.lon;
      UAV.altitude_msl = (int)mav_gps.alt_msl;
      UAV.height_agl = (int)mav_gps.alt_agl;
      UAV.heading = (int)mav_gps.hdg;
      UAV.band = detect_band;
      UAV.channel = detect_channel;
      strncpy(UAV.uav_id, "MAVLink", ODID_ID_SIZE);
      storeAndQueue(&UAV);
    }
    return;
  }

  // NAN Action Frame (WiFi Aware RemoteID)
  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0) {
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, nullptr, payload, length) == 0) {
      id_data UAV;
      memset(&UAV, 0, sizeof(UAV));
      memcpy(UAV.mac, &payload[10], 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();
      UAV.band = detect_band;
      UAV.channel = detect_channel;
      processODIDData(&UAV);
      storeAndQueue(&UAV);
    }
  }
  // Beacon Frame with RemoteID Vendor Specific IE
  else if (payload[0] == 0x80) {
    int offset = 36;
    while (offset + 1 < length) {
      int typ = payload[offset];
      int len = payload[offset + 1];
      if (offset + 2 + len > length) break;
      if (typ == 0xdd && len >= 4) {
        /* DJI DroneID OUI 26:37:12 */
        if (dji_is_oui(&payload[offset+2])) {
          dji_droneid_t dji;
          uint8_t src_mac[6];
          memcpy(src_mac, &payload[10], 6);
          if (dji_parse_droneid(&payload[offset + 5], len - 3, &dji)) {
            char djijson[384];
            dji_emit_json(src_mac, packet->rx_ctrl.rssi, &dji, djijson, sizeof(djijson));
            Serial.println(djijson);
            /* Full JSON (~300 B) exceeds Meshtastic MTU (~228 B); send compact relay instead */
            static unsigned long dji_last_mesh = 0;
            if (millis() - dji_last_mesh >= 5000) {
              char mesh_buf[200];
              int n = snprintf(mesh_buf, sizeof(mesh_buf),
                               "DJI %02x%02x%02x%02x%02x%02x RSSI:%d",
                               src_mac[0], src_mac[1], src_mac[2],
                               src_mac[3], src_mac[4], src_mac[5],
                               (int)packet->rx_ctrl.rssi);
              if (dji.lat != 0.0 && dji.lon != 0.0 && n < (int)sizeof(mesh_buf) - 2)
                n += snprintf(mesh_buf + n, sizeof(mesh_buf) - n,
                              " https://maps.google.com/?q=%.6f,%.6f", dji.lat, dji.lon);
              if (Serial1.availableForWrite() >= n + 2)
                Serial1.println(mesh_buf);
              dji_last_mesh = millis();
            }
          }
        }
        /* OpenDroneID OUIs FA:0B:BC and 90:3A:E6 */
        else if (((payload[offset+2] == 0x90 && payload[offset+3] == 0x3a && payload[offset+4] == 0xe6)) ||
                 ((payload[offset+2] == 0xfa && payload[offset+3] == 0x0b && payload[offset+4] == 0xbc))) {
          int j = offset + 7;
          if (j < length) {
            memset(&UAS_data, 0, sizeof(UAS_data));
            odid_message_process_pack(&UAS_data, &payload[j], length - j);

            id_data UAV;
            memset(&UAV, 0, sizeof(UAV));
            memcpy(UAV.mac, &payload[10], 6);
            UAV.rssi = packet->rx_ctrl.rssi;
            UAV.last_seen = millis();
            UAV.band = detect_band;
            UAV.channel = detect_channel;
            processODIDData(&UAV);
            storeAndQueue(&UAV);
          }
        }
      }
      offset += len + 2;
    }
  }
}

// ============================================================================
// Printer Task — outputs on both USB Serial and UART (mesh)
// ============================================================================

void printerTask(void *param) {
  id_data UAV;
  for (;;) {
    if (xQueueReceive(printQueue, &UAV, portMAX_DELAY)) {
      send_json_fast(&UAV);
      print_compact_message(&UAV);
    }
  }
}

// ============================================================================
// Initialization
// ============================================================================

void initializeSerial() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
  delay(100);

  Serial.println("\n========================================");
  Serial.println("    RemoteID Mesh Detect — Dual-Band");
  Serial.println("========================================");
  Serial.printf("Board: %s\n", BOARD_NAME);
#if DUAL_BAND_ENABLED
  Serial.println("Mode:  DUAL-BAND (2.4GHz + 5GHz WiFi)");
#else
  Serial.println("Mode:  SINGLE-BAND (2.4GHz WiFi only)");
#endif
  Serial.println("Proto: WiFi NAN, WiFi Beacon, BLE");
  Serial.printf("UART:  TX=GPIO%d, RX=GPIO%d → Heltec\n", SERIAL1_TX_PIN, SERIAL1_RX_PIN);
  Serial.println("========================================\n");
}

void setup() {
  setCpuFrequencyMhz(160);
  initializeSerial();

  nvs_flash_init();

  // Print queue — must exist before enabling promiscuous RX
  printQueue = xQueueCreate(MAX_UAVS, sizeof(id_data));

  // WiFi promiscuous mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);  /* initial; channelHopTask takes over */

#if DUAL_BAND_ENABLED
  Serial.println("WiFi promiscuous mode (2.4GHz ch1/6/11 + 5GHz ch149-165, hopping enabled)");
#else
  Serial.println("WiFi promiscuous mode (fixed ch6)");
#endif

  // BLE init (NimBLE 2.1.0)
  NimBLEDevice::init("DroneID");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  Serial.println("BLE scanning initialized (NimBLE)");

  // (printQueue already created before WiFi init above)

  // FreeRTOS tasks — C5 is single-core, S3 is dual-core
#if SINGLE_CORE
  xTaskCreate(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL);
  xTaskCreate(printerTask, "PrinterTask", 10000, NULL, 1, NULL);
  #if DUAL_BAND_ENABLED
  xTaskCreate(channelHopTask, "ChannelHopTask", 4096, NULL, 2, NULL);
  #endif
#else
  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(printerTask, "PrinterTask", 10000, NULL, 1, NULL, 1);
#endif

  memset(uavs, 0, sizeof(uavs));

  Serial.println("\n[+] Scanning for drones...\n");
}

// ============================================================================
// Main Loop
// ============================================================================

void loop() {
  unsigned long current_millis = millis();

  if ((current_millis - last_status) > 60000UL) {
#if DUAL_BAND_ENABLED
    Serial.println("{\"status\":\"active\",\"mode\":\"dual-band\",\"bands\":[\"2.4GHz\",\"5GHz\",\"BLE\"]}");
#else
    Serial.println("{\"status\":\"active\",\"mode\":\"single-band\",\"bands\":[\"2.4GHz\",\"BLE\"]}");
#endif
    last_status = current_millis;
  }
}
