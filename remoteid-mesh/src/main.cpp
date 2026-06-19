

/* Minimal scanner for WiFi direct remote ID */

#if !defined(ARDUINO_ARCH_ESP32)
  #error "This program requires an ESP32"
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "opendroneid.h"
#include "odid_wifi.h"
#include "dji_droneid.h"
#include "bt_odid.h"
#include "mavlink_wifi.h"

// Custom UART pin definitions for Serial1
const int SERIAL1_RX_PIN = 7;  // GPIO7
const int SERIAL1_TX_PIN = 6;  // GPIO6

ODID_UAS_Data UAS_data;

// Structure to hold UAV detection data
struct uav_data {
  uint8_t mac[6];
  uint8_t padding[1];
  int8_t rssi;
  char op_id[ODID_ID_SIZE + 1];
  char uav_id[ODID_ID_SIZE + 1];
  double lat_d;
  double long_d;
  double base_lat_d;
  double base_long_d;
  int altitude_msl;
  int height_agl;
  int speed;
  int heading;
  int speed_vertical;
  int altitude_pressure;
  int horizontal_accuracy;
  int vertical_accuracy;
  int baro_accuracy;
  int speed_accuracy;
  int timestamp;
  int status;
  int height_type;
  int operator_location_type;
  int classification_type;
  int area_count;
  int area_radius;
  int area_ceiling;
  int area_floor;
  int operator_altitude_geo;
  uint32_t system_timestamp;
  int operator_id_type;
  uint8_t ua_type;
  uint8_t auth_type;
  uint8_t auth_page;
  uint8_t auth_length;
  uint32_t auth_timestamp;
  char auth_data[ODID_AUTH_PAGE_NONZERO_DATA_SIZE + 1];
  uint8_t desc_type;
  char description[ODID_STR_SIZE + 1];
};

// Forward declarations
void event_handler(void *ctx, esp_event_base_t event_base, int32_t event_id, void *event_data);
void callback(void *, wifi_promiscuous_pkt_type_t);
void parse_odid(struct uav_data *, ODID_UAS_Data *);
void store_mac(struct uav_data *uav, uint8_t *payload);
void send_json_fast(struct uav_data *UAV);
void print_compact_message(struct uav_data *UAV);

unsigned long last_status = 0;

static BLEScan *pBLEScan = nullptr;
static SemaphoreHandle_t g_serial_mux = nullptr;

static inline void serial_println_locked(const char *s) {
  xSemaphoreTake(g_serial_mux, pdMS_TO_TICKS(100));
  Serial.println(s);
  xSemaphoreGive(g_serial_mux);
}

void event_handler(void *ctx, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // No-op handler for now
}

// BLE callback: decodes a single ODID message from each advertisement.
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice device) override {
    int adv_len = device.getPayloadLength();
    if (adv_len <= 0) return;
    uint8_t *adv = device.getPayload();

    const uint8_t *msgs;
    int msgs_len;
    if (!bt_odid_find_odid(adv, adv_len, &msgs, &msgs_len) || msgs_len < 1) return;

    uav_data uav_buf = {};
    uav_data *UAV = &uav_buf;

    uint8_t *mac = (uint8_t *)device.getAddress().getNative();
    memcpy(UAV->mac, mac, 6);
    UAV->rssi = device.getRSSI();

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

    send_json_fast(UAV);
    print_compact_message(UAV);
  }
};

void bleScanTask(void *parameter) {
  for (;;) {
    pBLEScan->start(1, false);
    pBLEScan->clearResults();
    delay(100);
  }
}

// Initialize USB Serial (for JSON output) and Serial1 (
void initializeSerial() {
  // Initialize USB Serial for JSON payloads.
  Serial.begin(115200);
  // Initialize Serial1 for mesh detection messages.
  Serial1.begin(115200, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
  Serial.println("USB Serial (for JSON) and UART (Serial1) initialized.");
}

void setup() {
  setCpuFrequencyMhz(160);
  nvs_flash_init();
  esp_netif_init();  // Modern replacement for tcpip_adapter_init
  initializeSerial();
  esp_event_loop_create_default();  // Modern replacement
  esp_event_handler_instance_register(ESP_EVENT_ANY_BASE, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();
  g_serial_mux = xSemaphoreCreateMutex();

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);

  BLEDevice::init("DroneID");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL, 1);
}

void loop() {
  delay(10);
  unsigned long current_millis = millis();
  if ((current_millis - last_status) > 60000UL) {
    serial_println_locked("{\"heartbeat\":\"Device is active and running.\"}");
    last_status = current_millis;
  }
}

void send_json_fast(struct uav_data *UAV) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);
  char json_msg[320];
  snprintf(json_msg, sizeof(json_msg),
    "{\"mac\":\"%s\", \"rssi\":%d, \"drone_lat\":%.6f, \"drone_long\":%.6f, \"drone_altitude\":%d, \"pilot_lat\":%.6f, \"pilot_long\":%.6f, \"basic_id\":\"%s\"}",
    mac_str, UAV->rssi, UAV->lat_d, UAV->long_d, UAV->altitude_msl, UAV->base_lat_d, UAV->base_long_d, UAV->uav_id);
  serial_println_locked(json_msg);
}

// Sends UART messages over Serial1 exactly as before.
void print_compact_message(struct uav_data *UAV) {
  static unsigned long lastSendTime = 0;
  const unsigned long sendInterval = 5000;  // 5-second interval for UART messages
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
                      "Drone: %s RSSI:%d", mac_str, UAV->rssi);
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

// WiFi promiscuous callback: processes packets and sends both UART and fast JSON.
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
      uav_data UAV = {};
      memcpy(UAV.mac, mav_mac, 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.lat_d = mav_gps.lat;
      UAV.long_d = mav_gps.lon;
      UAV.altitude_msl = (int)mav_gps.alt_msl;
      UAV.height_agl = (int)mav_gps.alt_agl;
      UAV.heading = (int)mav_gps.hdg;
      strncpy(UAV.uav_id, "MAVLink", ODID_ID_SIZE);
      send_json_fast(&UAV);
      print_compact_message(&UAV);
    }
    return;
  }

  uav_data UAV = {};
  
  store_mac(&UAV, payload);
  UAV.rssi = packet->rx_ctrl.rssi;
  
  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0) {
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data,
                                                          (char *)UAV.op_id,
                                                          payload, length) == 0) {
      parse_odid(&UAV, &UAS_data);
      print_compact_message(&UAV);
      send_json_fast(&UAV);         // Send JSON messages as fast as possible.
    }
  }
  else if (payload[0] == 0x80) {
    int offset = 36;
    bool printed = false;
    while (offset + 1 < length) {
      int typ = payload[offset];
      int len = payload[offset + 1];
      if (offset + 2 + len > length) break;
      if (!printed && typ == 0xdd && len >= 4) {
        /* DJI DroneID OUI 26:37:12 */
        if (dji_is_oui(&payload[offset+2])) {
          dji_droneid_t dji;
          if (dji_parse_droneid(&payload[offset + 5], len - 3, &dji)) {
            char djijson[384];
            dji_emit_json(UAV.mac, UAV.rssi, &dji, djijson, sizeof(djijson));
            serial_println_locked(djijson);
            /* Full JSON (~300 B) exceeds Meshtastic MTU (~228 B); send compact relay instead */
            static unsigned long dji_last_mesh = 0;
            if (millis() - dji_last_mesh >= 5000) {
              char mesh_buf[200];
              int n = snprintf(mesh_buf, sizeof(mesh_buf),
                               "DJI %02x%02x%02x%02x%02x%02x RSSI:%d",
                               UAV.mac[0], UAV.mac[1], UAV.mac[2],
                               UAV.mac[3], UAV.mac[4], UAV.mac[5],
                               UAV.rssi);
              if (dji.lat != 0.0 && dji.lon != 0.0 && n < (int)sizeof(mesh_buf) - 2)
                n += snprintf(mesh_buf + n, sizeof(mesh_buf) - n,
                              " https://maps.google.com/?q=%.6f,%.6f", dji.lat, dji.lon);
              if (Serial1.availableForWrite() >= n + 2)
                Serial1.println(mesh_buf);
              dji_last_mesh = millis();
            }
            printed = true;
          }
        }
        /* OpenDroneID OUIs FA:0B:BC and 90:3A:E6 */
        else if (((payload[offset+2] == 0x90 && payload[offset+3] == 0x3a && payload[offset+4] == 0xe6)) ||
                 ((payload[offset+2] == 0xfa && payload[offset+3] == 0x0b && payload[offset+4] == 0xbc))) {
          int j = offset + 7;
          if (j < length) {
            memset(&UAS_data, 0, sizeof(UAS_data));
            odid_message_process_pack(&UAS_data, &payload[j], length - j);
            parse_odid(&UAV, &UAS_data);
            print_compact_message(&UAV);
            send_json_fast(&UAV);
            printed = true;
          }
        }
      }
      offset += len + 2;
    }
  }
}

void parse_odid(uav_data *UAV, ODID_UAS_Data *UAS_data2) {
  memset(UAV->op_id, 0, sizeof(UAV->op_id));
  memset(UAV->uav_id, 0, sizeof(UAV->uav_id));
  memset(UAV->description, 0, sizeof(UAV->description));
  memset(UAV->auth_data, 0, sizeof(UAV->auth_data));
  
  if (UAS_data2->BasicIDValid[0]) {
    strncpy(UAV->uav_id, (char *)UAS_data2->BasicID[0].UASID, ODID_ID_SIZE);
    UAV->uav_id[ODID_ID_SIZE] = '\0';
  }
  if (UAS_data2->LocationValid) {
    UAV->lat_d = UAS_data2->Location.Latitude;
    UAV->long_d = UAS_data2->Location.Longitude;
    UAV->altitude_msl = (int)UAS_data2->Location.AltitudeGeo;
    UAV->height_agl = (int)UAS_data2->Location.Height;
    UAV->speed = (int)UAS_data2->Location.SpeedHorizontal;
    UAV->heading = (int)UAS_data2->Location.Direction;
    UAV->speed_vertical = (int)UAS_data2->Location.SpeedVertical;
    UAV->altitude_pressure = (int)UAS_data2->Location.AltitudeBaro;
    UAV->height_type = UAS_data2->Location.HeightType;
    UAV->horizontal_accuracy = UAS_data2->Location.HorizAccuracy;
    UAV->vertical_accuracy = UAS_data2->Location.VertAccuracy;
    UAV->baro_accuracy = UAS_data2->Location.BaroAccuracy;
    UAV->speed_accuracy = UAS_data2->Location.SpeedAccuracy;
    UAV->timestamp = (int)UAS_data2->Location.TimeStamp;
    UAV->status = UAS_data2->Location.Status;
  }
  if (UAS_data2->SystemValid) {
    UAV->base_lat_d = UAS_data2->System.OperatorLatitude;
    UAV->base_long_d = UAS_data2->System.OperatorLongitude;
    UAV->operator_location_type = UAS_data2->System.OperatorLocationType;
    UAV->classification_type = UAS_data2->System.ClassificationType;
    UAV->area_count = UAS_data2->System.AreaCount;
    UAV->area_radius = UAS_data2->System.AreaRadius;
    UAV->area_ceiling = UAS_data2->System.AreaCeiling;
    UAV->area_floor = UAS_data2->System.AreaFloor;
    UAV->operator_altitude_geo = UAS_data2->System.OperatorAltitudeGeo;
    UAV->system_timestamp = UAS_data2->System.Timestamp;
  }
  if (UAS_data2->AuthValid[0]) {
    UAV->auth_type = UAS_data2->Auth[0].AuthType;
    UAV->auth_page = UAS_data2->Auth[0].DataPage;
    UAV->auth_length = UAS_data2->Auth[0].Length;
    UAV->auth_timestamp = UAS_data2->Auth[0].Timestamp;
    memcpy(UAV->auth_data, UAS_data2->Auth[0].AuthData, sizeof(UAV->auth_data) - 1);
  }
  if (UAS_data2->SelfIDValid) {
    UAV->desc_type = UAS_data2->SelfID.DescType;
    strncpy(UAV->description, UAS_data2->SelfID.Desc, ODID_STR_SIZE);
  }
  if (UAS_data2->OperatorIDValid) {
    UAV->operator_id_type = UAS_data2->OperatorID.OperatorIdType;
    strncpy(UAV->op_id, (char *)UAS_data2->OperatorID.OperatorId, ODID_ID_SIZE);
  }
}

void store_mac(uav_data *uav, uint8_t *payload) {
  memcpy(uav->mac, &payload[10], 6);
}
