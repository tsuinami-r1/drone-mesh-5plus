#if !defined(ARDUINO_ARCH_ESP32)
  #error "This program requires an ESP32S3"
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
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

const int SERIAL1_RX_PIN = 6;
const int SERIAL1_TX_PIN = 5;

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
};

void callback(void *, wifi_promiscuous_pkt_type_t);
void send_json_fast(const id_data *UAV);
void print_compact_message(const id_data *UAV);

#define MAX_UAVS 8
id_data uavs[MAX_UAVS] = {0};
BLEScan* pBLEScan = nullptr;
ODID_UAS_Data UAS_data;
unsigned long last_status = 0;

static QueueHandle_t printQueue;

id_data* next_uav(uint8_t* mac) {
  for (int i = 0; i < MAX_UAVS; i++) {
    if (memcmp(uavs[i].mac, mac, 6) == 0)
      return &uavs[i];
  }
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].mac[0] == 0)
      return &uavs[i];
  }
  return &uavs[0];
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice device) override {
    int adv_len = device.getPayloadLength();
    if (adv_len <= 0) return;
    uint8_t *adv = device.getPayload();

    const uint8_t *msgs;
    int msgs_len;
    if (!bt_odid_find_odid(adv, adv_len, &msgs, &msgs_len) || msgs_len < 1) return;

    uint8_t *mac = (uint8_t *)device.getAddress().getNative();
    id_data *UAV = next_uav(mac);
    UAV->last_seen = millis();
    UAV->rssi = device.getRSSI();
    memcpy(UAV->mac, mac, 6);

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

void send_json_fast(const id_data *UAV) {
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);
  char json_msg[320];
  snprintf(json_msg, sizeof(json_msg),
    "{\"mac\":\"%s\",\"rssi\":%d,\"drone_lat\":%.6f,\"drone_long\":%.6f,\"drone_altitude\":%d,\"pilot_lat\":%.6f,\"pilot_long\":%.6f,\"basic_id\":\"%s\"}",
    mac_str, UAV->rssi, UAV->lat_d, UAV->long_d, UAV->altitude_msl,
    UAV->base_lat_d, UAV->base_long_d, UAV->uav_id);
  Serial.println(json_msg);
}

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

void bleScanTask(void *parameter) {
  for (;;) {
    pBLEScan->start(1, false);
    pBLEScan->clearResults();
    delay(100);
  }
}

static void storeAndQueue(id_data *UAV) {
  id_data *storedUAV = next_uav(UAV->mac);
  *storedUAV = *UAV;
  storedUAV->flag = 1;
  id_data tmp = *storedUAV;
  xQueueSend(printQueue, &tmp, 0);
}

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
      id_data UAV = {};
      memcpy(UAV.mac, mav_mac, 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();
      UAV.lat_d = mav_gps.lat;
      UAV.long_d = mav_gps.lon;
      UAV.altitude_msl = (int)mav_gps.alt_msl;
      UAV.height_agl = (int)mav_gps.alt_agl;
      UAV.heading = (int)mav_gps.hdg;
      strncpy(UAV.uav_id, "MAVLink", ODID_ID_SIZE);
      storeAndQueue(&UAV);
    }
    return;
  }

  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0) {
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, nullptr, payload, length) == 0) {
      id_data UAV;
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
      
      storeAndQueue(&UAV);
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

            storeAndQueue(&UAV);
          }
        }
      }
      offset += len + 2;
    }
  }
}

void printerTask(void *param) {
  id_data UAV;
  for (;;) {
    if (xQueueReceive(printQueue, &UAV, portMAX_DELAY)) {
      send_json_fast(&UAV);
      print_compact_message(&UAV);
    }
  }
}

void initializeSerial() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
}

void setup() {
  setCpuFrequencyMhz(160);
  initializeSerial();
  nvs_flash_init();
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  BLEDevice::init("DroneID");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);

  printQueue = xQueueCreate(MAX_UAVS, sizeof(id_data));

  /* Enable promiscuous mode AFTER printQueue is created so the callback
   * never calls xQueueSend on a NULL handle. */
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);

  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(printerTask, "PrinterTask", 10000, NULL, 1, NULL, 1);

  memset(uavs, 0, sizeof(uavs));
}

void loop() {
  unsigned long current_millis = millis();
    if ((current_millis - last_status) > 60000UL) {
      Serial.println("{\"heartbeat\":\"Device is active and scanning.\"}");
      last_status = current_millis;
    }
}
