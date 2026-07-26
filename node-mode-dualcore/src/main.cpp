/*
* node-mode with dual Wi-Fi and BLE support for ESP32S3
*/
#if !defined(ARDUINO_ARCH_ESP32)
#error "This program requires an ESP32"
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "opendroneid.h"
#include "odid_wifi.h"
#include "dji_droneid.h"
#include "bt_odid.h"
#include "mavlink_wifi.h"
#include <esp_timer.h>

// UART pin definitions for Serial1 on esp32s3
const int SERIAL1_RX_PIN = 6;  // GPIO6
const int SERIAL1_TX_PIN = 5;  // GPIO5

// Structure to hold UAV detection data
struct uav_data {
  uint8_t  mac[6];
  int      rssi;
  uint32_t last_seen;   // millis() of last update; 0 == slot free (occupancy flag)
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
};

#define MAX_UAVS 8
uav_data uavs[MAX_UAVS] = {0};
static unsigned long last_mesh_ms[MAX_UAVS] = {0};  // per-slot mesh rate limiter
BLEScan* pBLEScan = nullptr;
ODID_UAS_Data UAS_data;
unsigned long last_status = 0;

// uavs[] is written from the WiFi promiscuous callback and the BLE callback and
// read/cleared from bleScanTask — three contexts, so guard it. Serial/Serial1
// are written from several contexts too. The two mutexes are never held nested
// (the uav lock is always released before any serial write), so no deadlock.
static SemaphoreHandle_t g_uav_mux    = nullptr;
static SemaphoreHandle_t g_serial_mux = nullptr;

// Forward declarations
void callback(void *, wifi_promiscuous_pkt_type_t);
void send_json_fast(const uav_data *UAV);
void print_compact_message(const uav_data *UAV, int slot);

static inline void serial_println_locked(const char *s) {
  if (g_serial_mux == nullptr) { Serial.println(s); return; }
  if (xSemaphoreTake(g_serial_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    Serial.println(s);
    xSemaphoreGive(g_serial_mux);
  }
}

static inline void serial1_println_locked(const char *s, int min_free) {
  if (g_serial_mux == nullptr) {
    if (Serial1.availableForWrite() >= min_free) Serial1.println(s);
    return;
  }
  if (xSemaphoreTake(g_serial_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (Serial1.availableForWrite() >= min_free) Serial1.println(s);
    xSemaphoreGive(g_serial_mux);
  }
}

// Get the slot for this MAC, or claim a free/oldest slot for a new one.
// Occupancy is tracked by last_seen != 0 so drones whose real MAC begins with
// 0x00 (common OUIs) are not mistaken for empty slots. A newly claimed slot is
// cleared so a previous occupant's coordinates/ID cannot bleed into the new one.
// MUST be called with g_uav_mux held.
uav_data* next_uav(uint8_t* mac) {
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].last_seen != 0 && memcmp(uavs[i].mac, mac, 6) == 0)
      return &uavs[i];
  }
  int idx = -1;
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].last_seen == 0) { idx = i; break; }
  }
  if (idx < 0) {
    uint32_t oldest = UINT32_MAX;
    idx = 0;
    for (int i = 0; i < MAX_UAVS; i++) {
      if (uavs[i].last_seen < oldest) { oldest = uavs[i].last_seen; idx = i; }
    }
  }
  memset(&uavs[idx], 0, sizeof(uavs[idx]));
  memcpy(uavs[idx].mac, mac, 6);
  uavs[idx].last_seen = millis();
  last_mesh_ms[idx] = 0;   // allow the new occupant to relay immediately
  return &uavs[idx];
}

// BLE Advertisement callback handler
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice device) override {
    int adv_len = device.getPayloadLength();
    if (adv_len <= 0) return;
    uint8_t *adv = device.getPayload();

    const uint8_t *msgs;
    int msgs_len;
    if (!bt_odid_find_odid(adv, adv_len, &msgs, &msgs_len)) return;
    // Decoders read a full 25-byte ODID message; require that many bytes so a
    // truncated advertisement cannot cause an out-of-bounds read.
    if (msgs_len < ODID_MESSAGE_SIZE) return;

    uint8_t *mac = (uint8_t *)device.getAddress().getNative();
    int rssi = device.getRSSI();
    const uint8_t *odid = msgs;

    xSemaphoreTake(g_uav_mux, portMAX_DELAY);
    uav_data *UAV = next_uav(mac);
    UAV->last_seen = millis();
    UAV->rssi = rssi;
    memcpy(UAV->mac, mac, 6);

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
    xSemaphoreGive(g_uav_mux);
  }
};

// Initialize USB Serial (for JSON output) and Serial1 (for mesh/UART)
void initializeSerial() {
  Serial.begin(115200);
  // Larger RX ring so bursty Meshtastic relay traffic is not dropped between
  // uartForwardTask polls (must be set before begin()).
  Serial1.setRxBufferSize(1024);
  Serial1.begin(115200, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
  Serial.println("USB Serial (for JSON) and UART (Serial1) initialized.");
}

// Sends JSON payload as fast as possible over USB Serial (includes basic_id).
void send_json_fast(const uav_data *UAV) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);
  char id_esc[48];  /* uav_id is raw over-the-air bytes — escape for JSON safety */
  dji_escape_str(UAV->uav_id, id_esc, sizeof(id_esc));
  char json_msg[256];
  snprintf(json_msg, sizeof(json_msg),
    "{\"mac\":\"%s\",\"rssi\":%d,\"drone_lat\":%.6f,\"drone_long\":%.6f,"
    "\"drone_altitude\":%d,\"pilot_lat\":%.6f,\"pilot_long\":%.6f,"
    "\"basic_id\":\"%s\"}",
    mac_str, UAV->rssi, UAV->lat_d, UAV->long_d, UAV->altitude_msl,
    UAV->base_lat_d, UAV->base_long_d, id_esc);
  serial_println_locked(json_msg);
}

// Emits single combined JSON message over Serial1 (aligned with mesh-mapper.py API).
// Rate limited per slot so one drone cannot starve another's mesh relay.
void print_compact_message(const uav_data *UAV, int slot) {
  const unsigned long sendInterval = 3000;  // 3-second interval per drone
  if (slot < 0 || slot >= MAX_UAVS) return;
  if (millis() - last_mesh_ms[slot] < sendInterval) return;
  last_mesh_ms[slot] = millis();

  // Format MAC address
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);

  // Single combined JSON message matching mesh-mapper.py API expectations
  char id_esc[48];  /* uav_id is raw over-the-air bytes — escape for JSON safety */
  dji_escape_str(UAV->uav_id, id_esc, sizeof(id_esc));
  char json_msg[256];
  int len_msg = snprintf(json_msg, sizeof(json_msg),
    "{\"mac\":\"%s\",\"rssi\":%d,\"drone_lat\":%.6f,\"drone_long\":%.6f,"
    "\"drone_altitude\":%d,\"pilot_lat\":%.6f,\"pilot_long\":%.6f,"
    "\"basic_id\":\"%s\"}",
    mac_str, UAV->rssi, UAV->lat_d, UAV->long_d, UAV->altitude_msl,
    UAV->base_lat_d, UAV->base_long_d, id_esc);

  serial1_println_locked(json_msg, len_msg);
}

// Wi-Fi promiscuous packet callback
void callback(void *buffer, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

  wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buffer;
  uint8_t *payload = packet->payload;
  int length = packet->rx_ctrl.sig_len;

  if (length < 16) return;   /* too short to safely read addr/NAN fields */

  if (type == WIFI_PKT_DATA) {
    uint8_t mav_mac[6];
    mav_gps_t mav_gps;
    if (mav_wifi_extract(payload, length, mav_mac, &mav_gps)) {
      xSemaphoreTake(g_uav_mux, portMAX_DELAY);
      uav_data *UAV = next_uav(mav_mac);
      memcpy(UAV->mac, mav_mac, 6);
      UAV->rssi = packet->rx_ctrl.rssi;
      UAV->last_seen = millis();
      UAV->lat_d = mav_gps.lat;
      UAV->long_d = mav_gps.lon;
      UAV->altitude_msl = (int)mav_gps.alt_msl;
      UAV->height_agl = (int)mav_gps.alt_agl;
      UAV->heading = (int)mav_gps.hdg;
      strncpy(UAV->uav_id, "MAVLink", ODID_ID_SIZE);
      UAV->flag = 1;
      xSemaphoreGive(g_uav_mux);
    }
    return;
  }

  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0) {
    /* Parser unconditionally writes the 6-byte source MAC to this arg; must be a
       real buffer, not nullptr (UAV.mac is re-derived from &payload[10] below). */
    uint8_t nan_src[6];
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, (char *)nan_src, payload, length) == 0) {
      uav_data UAV;
      memset(&UAV, 0, sizeof(UAV));
      memcpy(UAV.mac, &payload[10], 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();

      if (UAS_data.BasicIDValid[0]) {
        strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
        UAV.uav_id[ODID_ID_SIZE] = '\0';
      }
      if (UAS_data.LocationValid) {
        UAV.lat_d = UAS_data.Location.Latitude;
        UAV.long_d = UAS_data.Location.Longitude;
        UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
        UAV.height_agl = (int)UAS_data.Location.Height;
        UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
        UAV.heading = (int)UAS_data.Location.Direction;
      }
      if (UAS_data.SystemValid) {
        UAV.base_lat_d = UAS_data.System.OperatorLatitude;
        UAV.base_long_d = UAS_data.System.OperatorLongitude;
      }
      if (UAS_data.OperatorIDValid) {
        strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
      }

      xSemaphoreTake(g_uav_mux, portMAX_DELAY);
      uav_data* dbUAV = next_uav(UAV.mac);
      memcpy(dbUAV, &UAV, sizeof(UAV));
      dbUAV->flag = 1;
      xSemaphoreGive(g_uav_mux);
    }
  }
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
            serial_println_locked(djijson);
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
              serial1_println_locked(mesh_buf, n + 2);
              dji_last_mesh = millis();
            }
          }
        }
        /* OpenDroneID OUIs FA:0B:BC and 90:3A:E6 */
        else if (((payload[offset+2] == 0x90 && payload[offset+3] == 0x3a && payload[offset+4] == 0xe6)) ||
                 ((payload[offset+2] == 0xfa && payload[offset+3] == 0x0b && payload[offset+4] == 0xbc))) {
          int j = offset + 7;
          /* Bound the ODID pack to this IE's body (len - 5 bytes after the OUI +
             vendor-type header), not the rest of the frame, so a truncated IE
             cannot make the decoder consume following IEs as ODID messages. */
          if (len >= 6 && j < length) {
            int pack_len = len - 5;
            memset(&UAS_data, 0, sizeof(UAS_data));
            odid_message_process_pack(&UAS_data, &payload[j], pack_len);

            uav_data UAV;
            memset(&UAV, 0, sizeof(UAV));
            memcpy(UAV.mac, &payload[10], 6);
            UAV.rssi = packet->rx_ctrl.rssi;
            UAV.last_seen = millis();

            if (UAS_data.BasicIDValid[0]) {
              strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
              UAV.uav_id[ODID_ID_SIZE] = '\0';
            }
            if (UAS_data.LocationValid) {
              UAV.lat_d = UAS_data.Location.Latitude;
              UAV.long_d = UAS_data.Location.Longitude;
              UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
              UAV.height_agl = (int)UAS_data.Location.Height;
              UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
              UAV.heading = (int)UAS_data.Location.Direction;
            }
            if (UAS_data.SystemValid) {
              UAV.base_lat_d = UAS_data.System.OperatorLatitude;
              UAV.base_long_d = UAS_data.System.OperatorLongitude;
            }
            if (UAS_data.OperatorIDValid) {
              strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
            }

            xSemaphoreTake(g_uav_mux, portMAX_DELAY);
            uav_data* dbUAV = next_uav(UAV.mac);
            memcpy(dbUAV, &UAV, sizeof(UAV));
            dbUAV->flag = 1;
            xSemaphoreGive(g_uav_mux);
          }
        }
      }
      offset += len + 2;
    }
  }
}

void bleScanTask(void *parameter) {
  for(;;) {
    pBLEScan->start(1, false);
    pBLEScan->clearResults();

    for (int i = 0; i < MAX_UAVS; i++) {
      // Snapshot the slot under the lock, then emit outside it so blocking
      // serial writes never run while holding the uav mutex.
      uav_data snap;
      bool pending = false;
      xSemaphoreTake(g_uav_mux, portMAX_DELAY);
      if (uavs[i].last_seen != 0 && uavs[i].flag) {
        snap = uavs[i];
        uavs[i].flag = 0;
        pending = true;
      }
      xSemaphoreGive(g_uav_mux);
      if (pending) {
        send_json_fast(&snap);
        print_compact_message(&snap, i);
      }
    }

    unsigned long current_millis = millis();
    if ((current_millis - last_status) > 60000UL) {
      serial_println_locked("{\"heartbeat\":\"Device is active and running.\"}");
      last_status = current_millis;
    }

    delay(100);
  }
}

static const uint8_t channels_2_4ghz[] = {1, 6, 11};

void channelHopTask(void *parameter) {
  uint8_t idx = 0;
  for (;;) {
    esp_wifi_set_channel(channels_2_4ghz[idx], WIFI_SECOND_CHAN_NONE);
    idx = (idx + 1) % (sizeof(channels_2_4ghz) / sizeof(channels_2_4ghz[0]));
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// Task to forward incoming JSON from Serial1 (UART) to USB Serial.
// Line-buffered and emitted whole under the serial mutex so a forwarded mesh
// line cannot interleave mid-line with locally-printed detection JSON.
void uartForwardTask(void *parameter) {
  static char lineBuf[512];
  int pos = 0;
  for (;;) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (c == '\n' || c == '\r') {
        if (pos > 0) {
          lineBuf[pos] = '\0';
          serial_println_locked(lineBuf);
          pos = 0;
        }
      } else if (pos < (int)sizeof(lineBuf) - 1) {
        lineBuf[pos++] = c;
      } else {
        // Line too long — flush what we have to avoid losing byte sync.
        lineBuf[pos] = '\0';
        serial_println_locked(lineBuf);
        pos = 0;
        lineBuf[pos++] = c;
      }
    }
    delay(5);  // fast poll so the 1 KB RX buffer never overflows
  }
}

void setup() {
  delay(6000);  // 6-second boot delay (necessary for xiao meshtastic)
  setCpuFrequencyMhz(160);

  // Create mutexes before promiscuous RX / BLE / tasks can touch shared state.
  g_uav_mux    = xSemaphoreCreateMutex();
  g_serial_mux = xSemaphoreCreateMutex();

  nvs_flash_init();
  initializeSerial();

  // Initialize Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);

  // Initialize BLE scanning
  BLEDevice::init("DroneID");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  // Initialize UAV tracking array
  memset(uavs, 0, sizeof(uavs));

  xTaskCreatePinnedToCore(bleScanTask,    "BLEScanTask",    10000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(channelHopTask, "ChannelHopTask", 2048,  NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(uartForwardTask, "UARTForwardTask", 4096, NULL, 1, NULL, 1);
}

void loop() {
  // Main tasks are handled by the FreeRTOS tasks on separate cores
}
