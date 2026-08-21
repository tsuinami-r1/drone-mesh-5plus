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
#include <freertos/semphr.h>

// ============================================================================
// UART Pins — same wiring as remoteid-mesh-dualcore (Heltec LoRa V3)
// ============================================================================

const int SERIAL1_TX_PIN = 5;   // GPIO5 → Heltec RX
const int SERIAL1_RX_PIN = 6;   // GPIO6 → Heltec TX

// ============================================================================
// Regulatory domain — set to match the deployment jurisdiction
// ============================================================================
// Bounds which channels esp_wifi_set_channel() accepts, and therefore which
// channels the scan schedule can actually reach. Nodes are receive-only, so
// this carries no transmit implications.
//   US / FCC       : start 1, count 11
//   EU / HK / ETSI : start 1, count 13   <- default
//   Japan          : start 1, count 14   (ch 14 is 802.11b DSSS only)
// Prefer over-declaring: a channel the hardware refuses is skipped harmlessly
// (and logged), but a channel excluded here is never scanned at all.
#define WIFI_COUNTRY_CC   "HK"
#define WIFI_CHAN_START   1
#define WIFI_CHAN_COUNT   13

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
// Channel Scan Schedule
// ============================================================================

// Two-tier schedule: every primary channel is visited each cycle, plus ONE
// secondary channel sampled round-robin. Scanning only 1/6/11 leaves 10 of the
// 13 permitted 2.4 GHz channels permanently unscanned — drones pick channels
// dynamically and are not bound to the consumer 1/6/11 convention. Tiering adds
// full-band coverage while keeping a fast revisit on the common channels.
#if BOARD_IS_C5
  // Dual-band: 2.4 GHz primaries + 5 GHz UNII-3
  static const uint8_t primary_channels[] = {1, 6, 11, 149, 153, 157, 161, 165};
  #define DWELL_TIME_MS 50    // cycle ≈ 450 ms; each secondary revisited ≈ 4.5 s
#else
  // S3 is 2.4 GHz only
  static const uint8_t primary_channels[] = {1, 6, 11};
  #define DWELL_TIME_MS 200   // cycle ≈ 800 ms; each secondary revisited ≈ 8 s
#endif
static const uint8_t secondary_channels[] = {2, 3, 4, 5, 7, 8, 9, 10, 12, 13};

#define NUM_PRIMARY_CHANNELS   (sizeof(primary_channels) / sizeof(primary_channels[0]))
#define NUM_SECONDARY_CHANNELS (sizeof(secondary_channels) / sizeof(secondary_channels[0]))

// Hit-triggered dwell extension. A drone's message set (BasicID / Location /
// System) arrives across several frames, so hopping away immediately after the
// first hit costs a full cycle before the rest can be collected.
//
// Scaled to the dwell so one definition suits both boards: the C5's fast
// schedule returns to a channel every ~450 ms and needs only a short hold,
// while the slower S3 schedule benefits from a longer one. Keep these small —
// the cap is spent PER CHANNEL, so an oversized value multiplies across every
// busy channel and would starve the 5 GHz revisit rate.
#define CHANNEL_HOLD_MS      (DWELL_TIME_MS * 6)   // C5 300 ms / S3 1200 ms
#define CHANNEL_HOLD_MAX_MS  (DWELL_TIME_MS * 8)   // C5 400 ms / S3 1600 ms

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

// Current channel tracking (updated by channelHopTask on both board targets)
volatile uint8_t current_channel = 6;  /* channelHopTask updates this */
volatile WiFiBand current_band = BAND_2_4GHZ;
static portMUX_TYPE channelMux = portMUX_INITIALIZER_UNLOCKED;

// millis() deadline for the hit-triggered dwell extension. A deadline in the
// past means "no hold" — never use 0 as the sentinel, see hop_to_channel().
static volatile uint32_t channel_hold_until = 0;

// Called from the WiFi promiscuous callback on a parsed detection so the hop
// task lingers here instead of moving on mid-message-set.
static inline void note_wifi_detection() {
  channel_hold_until = millis() + CHANNEL_HOLD_MS;
}

static QueueHandle_t printQueue;

// uavs[] is written from the WiFi promiscuous callback and the NimBLE scan
// callback; Serial/Serial1 are written from printerTask, the DJI branch, and
// loop(). Guard each with a mutex. The two are never held nested (the uav lock
// is released before any serial write), so there is no deadlock.
static SemaphoreHandle_t g_uav_mux    = nullptr;
static SemaphoreHandle_t g_serial_mux = nullptr;

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

// ============================================================================
// UAV Tracking
// ============================================================================

// MUST be called with g_uav_mux held. Occupancy is tracked by last_seen != 0 so
// drones with a 0x00-leading MAC are not mistaken for empty slots; a slot is
// cleared when reassigned so a previous occupant cannot bleed into the new one.
id_data* next_uav(const uint8_t* mac) {
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].last_seen != 0 && memcmp(uavs[i].mac, mac, 6) == 0)
      return &uavs[i];
  }
  int idx = -1;
  for (int i = 0; i < MAX_UAVS; i++) {
    if (uavs[i].last_seen == 0) { idx = i; break; }
  }
  if (idx < 0) {
    uint32_t oldest_time = UINT32_MAX;
    idx = 0;
    for (int i = 0; i < MAX_UAVS; i++) {
      if (uavs[i].last_seen < oldest_time) {
        oldest_time = uavs[i].last_seen;
        idx = i;
      }
    }
  }
  memset(&uavs[idx], 0, sizeof(uavs[idx]));
  memcpy(uavs[idx].mac, mac, 6);
  uavs[idx].last_seen = millis();
  return &uavs[idx];
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
    if (!bt_odid_find_odid(adv, adv_len, &msgs, &msgs_len)) return;
    // Decoders read a full 25-byte ODID message; require that many bytes so a
    // truncated advertisement cannot cause an out-of-bounds read.
    if (msgs_len < ODID_MESSAGE_SIZE) return;

    const uint8_t *mac = device->getAddress().getBase()->val;
    int rssi = device->getRSSI();
    const uint8_t *odid = msgs;

    xSemaphoreTake(g_uav_mux, portMAX_DELAY);
    id_data *UAV = next_uav(mac);
    UAV->last_seen = millis();
    UAV->rssi = rssi;
    memcpy(UAV->mac, mac, 6);
    UAV->band = BAND_BLE;
    UAV->channel = 0;

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
      case 0xF0: {
        // Message Pack — how BT5 Long Range / extended advertising (Coded PHY,
        // enabled on this build) carries Remote ID: several messages in one
        // advertisement. Decode into a LOCAL struct (never the shared UAS_data,
        // which the WiFi task owns) after bounding the pack to the bytes present.
        int pack_msgs = (msgs_len >= 3) ? odid[2] : 0;  // MsgPackSize
        if (pack_msgs >= 1 && pack_msgs <= ODID_PACK_MAX_MESSAGES &&
            msgs_len >= 3 + pack_msgs * ODID_MESSAGE_SIZE) {
          ODID_UAS_Data pack;
          memset(&pack, 0, sizeof(pack));
          if (decodeMessagePack(&pack, (ODID_MessagePack_encoded *)odid) == 0) {
            if (pack.BasicIDValid[0]) {
              strncpy(UAV->uav_id, (char *)pack.BasicID[0].UASID, ODID_ID_SIZE);
              UAV->uav_id[ODID_ID_SIZE] = '\0';
            }
            if (pack.LocationValid) {
              UAV->lat_d = pack.Location.Latitude;
              UAV->long_d = pack.Location.Longitude;
              UAV->altitude_msl = (int)pack.Location.AltitudeGeo;
              UAV->height_agl = (int)pack.Location.Height;
              UAV->speed = (int)pack.Location.SpeedHorizontal;
              UAV->heading = (int)pack.Location.Direction;
            }
            if (pack.SystemValid) {
              UAV->base_lat_d = pack.System.OperatorLatitude;
              UAV->base_long_d = pack.System.OperatorLongitude;
            }
            if (pack.OperatorIDValid) {
              strncpy(UAV->op_id, (char *)pack.OperatorID.OperatorId, ODID_ID_SIZE);
              UAV->op_id[ODID_ID_SIZE] = '\0';
            }
          }
        }
        break;
      }
    }
    UAV->flag = 1;
    id_data tmp = *UAV;
    xSemaphoreGive(g_uav_mux);
    xQueueSend(printQueue, &tmp, 0);
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
  char id_esc[48];  /* uav_id is raw over-the-air bytes — escape for JSON safety */
  dji_escape_str(UAV->uav_id, id_esc, sizeof(id_esc));
  char json_msg[320];
  snprintf(json_msg, sizeof(json_msg),
    "{\"mac\":\"%s\",\"rssi\":%d,\"band\":\"%s\",\"channel\":%d,"
    "\"drone_lat\":%.6f,\"drone_long\":%.6f,\"drone_altitude\":%d,"
    "\"pilot_lat\":%.6f,\"pilot_long\":%.6f,\"basic_id\":\"%s\"}",
    mac_str, UAV->rssi, bandToString(UAV->band), UAV->channel,
    UAV->lat_d, UAV->long_d, UAV->altitude_msl,
    UAV->base_lat_d, UAV->base_long_d, id_esc);
  serial_println_locked(json_msg);
}

// ============================================================================
// Compact Message Output (Serial1 UART → Heltec/Meshtastic)
// ============================================================================

// ---------------------------------------------------------------------------
// Per-drone mesh rate limit
// ---------------------------------------------------------------------------
// A single shared timer let whichever drone happened to hit the window first
// consume it, silently starving every other drone's relay — with several
// aircraft overhead the mesh reported only one of them. Key the limit on the
// source MAC instead so each drone gets its own interval.
#define MESH_RL_SLOTS 8
static uint8_t  mesh_rl_mac[MESH_RL_SLOTS][6] = {{0}};
static uint32_t mesh_rl_last[MESH_RL_SLOTS]   = {0};

static bool mesh_rate_allow(const uint8_t *mac, uint32_t interval_ms) {
  uint32_t now = millis();
  int free_idx = -1, oldest_idx = 0;
  for (int i = 0; i < MESH_RL_SLOTS; i++) {
    if (mesh_rl_last[i] != 0 && memcmp(mesh_rl_mac[i], mac, 6) == 0) {
      if (now - mesh_rl_last[i] < interval_ms) return false;   // rollover-safe
      mesh_rl_last[i] = now;
      return true;
    }
    if (mesh_rl_last[i] == 0 && free_idx < 0) free_idx = i;
    if (mesh_rl_last[i] < mesh_rl_last[oldest_idx]) oldest_idx = i;
  }
  int idx = (free_idx >= 0) ? free_idx : oldest_idx;   // evict least-recent
  memcpy(mesh_rl_mac[idx], mac, 6);
  mesh_rl_last[idx] = now;
  return true;
}

void print_compact_message(const id_data *UAV) {
  const int MAX_MESH_SIZE = 230;

  // Per-drone, not global — see mesh_rate_allow() above.
  if (!mesh_rate_allow(UAV->mac, 5000)) return;

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
  serial1_println_locked(mesh_msg, msg_len + 2);   // +2 for CRLF

  if (UAV->base_lat_d != 0.0 && UAV->base_long_d != 0.0) {
    char pilot_msg[MAX_MESH_SIZE];
    int pilot_len = snprintf(pilot_msg, sizeof(pilot_msg),
                             "Pilot[%s]: https://maps.google.com/?q=%.6f,%.6f",
                             mac_str, UAV->base_lat_d, UAV->base_long_d);
    serial1_println_locked(pilot_msg, pilot_len + 2);
  }
}

// ============================================================================
// Channel Hopping Task (both board targets)
// ============================================================================

// Tune to a channel, dwell, then linger while detections keep arriving here.
static void hop_to_channel(uint8_t ch) {
  WiFiBand band = (ch > 20) ? BAND_5GHZ : BAND_2_4GHZ;

  // A channel outside the configured regulatory domain is rejected; skip it
  // rather than burning a dwell on the previous channel and then mislabelling
  // the detections it yields. Warn a bounded number of times so a mis-set
  // domain is visible in the log instead of silently shrinking coverage.
  if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    static uint8_t warn_budget = 8;
    if (warn_budget) {
      warn_budget--;
      char w[104];
      snprintf(w, sizeof(w),
               "[SCAN] channel %u rejected by regulatory domain "
               "- check WIFI_CHAN_START/WIFI_CHAN_COUNT", (unsigned)ch);
      serial_println_locked(w);
    }
    return;
  }

  portENTER_CRITICAL(&channelMux);
  current_channel = ch;
  current_band = band;
  portEXIT_CRITICAL(&channelMux);

  // Fresh channel: never inherit the previous channel's hold. Use an
  // already-past deadline rather than 0 — the 0 sentinel is NOT rollover-safe,
  // since (int32_t)(0 - millis()) reads as "future" once uptime passes 24.9
  // days, which would make every hop linger the full cap for half of each
  // 49.7-day millis() cycle even with nothing detected.
  channel_hold_until = millis() - 1;
  vTaskDelay(pdMS_TO_TICKS(DWELL_TIME_MS));

  // Rollover-safe deadline compare: (int32_t)(deadline - now) > 0.
  uint32_t linger_start = millis();
  while ((int32_t)(channel_hold_until - millis()) > 0 &&
         (millis() - linger_start) < CHANNEL_HOLD_MAX_MS) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void channelHopTask(void *parameter) {
  Serial.printf("[SCAN] Channel hopping: %u primary + %u secondary, dwell %d ms\n",
                (unsigned)NUM_PRIMARY_CHANNELS, (unsigned)NUM_SECONDARY_CHANNELS,
                DWELL_TIME_MS);

  uint8_t sec = 0;
  for (;;) {
    for (unsigned i = 0; i < NUM_PRIMARY_CHANNELS; i++)
      hop_to_channel(primary_channels[i]);

    // One secondary channel per cycle, round-robin over the full band.
    hop_to_channel(secondary_channels[sec]);
    sec = (sec + 1) % NUM_SECONDARY_CHANNELS;
  }
}

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
  note_wifi_detection();   // linger on this channel for the rest of the message set
  xSemaphoreTake(g_uav_mux, portMAX_DELAY);
  id_data* storedUAV = next_uav(UAV->mac);
  *storedUAV = *UAV;
  storedUAV->flag = 1;
  id_data tmp = *storedUAV;
  xSemaphoreGive(g_uav_mux);
  xQueueSend(printQueue, &tmp, 0);
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
    /* Parser unconditionally writes the 6-byte source MAC to this arg; must be a
       real buffer, not nullptr (UAV.mac is re-derived from &payload[10] below). */
    uint8_t nan_src[6];
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, (char *)nan_src, payload, length) == 0) {
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
            note_wifi_detection();   // DJI bypasses storeAndQueue — hold here too
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

  // Mutexes must exist before promiscuous RX / BLE / tasks touch shared state.
  g_uav_mux    = xSemaphoreCreateMutex();
  g_serial_mux = xSemaphoreCreateMutex();

  // Print queue — must exist before enabling promiscuous RX
  printQueue = xQueueCreate(MAX_UAVS, sizeof(id_data));

  // WiFi promiscuous mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Regulatory domain — configured via WIFI_CHAN_* at the top of this file.
  // The IDF default domain stops at channel 11 and esp_wifi_set_channel()
  // rejects anything above it, which would drop 12/13 from the schedule.
  wifi_country_t ctry = {};
  strcpy(ctry.cc, WIFI_COUNTRY_CC);
  ctry.schan = WIFI_CHAN_START;
  ctry.nchan = WIFI_CHAN_COUNT;
  ctry.max_tx_power = 20;   // unused (receive-only) but must be valid
  ctry.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&ctry);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);  /* initial; channelHopTask takes over */

#if DUAL_BAND_ENABLED
  Serial.println("WiFi promiscuous mode (2.4GHz ch1-13 + 5GHz ch149-165, hopping enabled)");
#else
  Serial.println("WiFi promiscuous mode (2.4GHz ch1-13, hopping enabled)");
#endif

  // BLE init (NimBLE 2.1.0)
  NimBLEDevice::init("DroneID");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
#if CONFIG_BT_NIMBLE_EXT_ADV
  pBLEScan->setPhy(NimBLEScan::SCAN_ALL);  // 1M PHY + Coded PHY (BLE 5.0 long range)
#endif
  Serial.println("BLE scanning initialized (NimBLE)");

  // (printQueue already created before WiFi init above)

  // FreeRTOS tasks — C5 is single-core, S3 is dual-core
#if SINGLE_CORE
  xTaskCreate(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL);
  xTaskCreate(printerTask, "PrinterTask", 10000, NULL, 1, NULL);
  xTaskCreate(channelHopTask, "ChannelHopTask", 4096, NULL, 2, NULL);
#else
  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(printerTask, "PrinterTask", 10000, NULL, 1, NULL, 1);
  // S3 hops too — previously it sat on channel 6 for the life of the node.
  xTaskCreatePinnedToCore(channelHopTask, "ChannelHopTask", 4096, NULL, 2, NULL, 0);
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
    serial_println_locked("{\"status\":\"active\",\"mode\":\"dual-band\",\"bands\":[\"2.4GHz\",\"5GHz\",\"BLE\"]}");
#else
    serial_println_locked("{\"status\":\"active\",\"mode\":\"single-band\",\"bands\":[\"2.4GHz\",\"BLE\"]}");
#endif
    last_status = current_millis;
  }
}
