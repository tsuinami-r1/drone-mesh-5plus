/*
 * RX5808 5.8GHz Analog FM Detection
 * Supports: XIAO ESP32-S3  |  XIAO ESP32-C5
 *
 * Scans all 40 standard FPV channels (Raceband + Bands A/B/E/F) and emits a
 * JSON detection on USB Serial whenever RSSI exceeds the configured threshold,
 * indicating a nearby 5.8GHz analog FM video transmitter (FPV drone).
 *
 * Output
 *   USB Serial  — JSON lines consumed by mesh-mapper.py
 *   Serial1 UART — compact text for Heltec/Meshtastic relay
 *                  (set ENABLE_MESH_RELAY 0 to disable)
 *
 * Physical wiring (same D-pin labels on both boards — GPIO numbers differ):
 *
 *   Board         | D10 DATA | D8 CLK | D9 CS | D0 RSSI | D4 TX | D5 RX
 *   XIAO ESP32-S3 | GPIO9    | GPIO7  | GPIO8 | GPIO1   | GPIO5 | GPIO6
 *   XIAO ESP32-C5 | GPIO10   | GPIO8  | GPIO9 | GPIO2   | GPIO6 | GPIO7
 *
 *   3.3V → RX5808 VCC   |   GND → RX5808 GND
 *   D4  → Heltec RX     |   D5  ← Heltec TX   (optional mesh relay)
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include "rx5808.h"

// ============================================================
// Configuration
// ============================================================

// Set to 0 if not wiring a Heltec LoRa V3 for mesh relay
#define ENABLE_MESH_RELAY  1

// Board-specific UART pins for optional Heltec relay.
// Uses the same physical D4/D5 pins on both boards.
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(ARDUINO_XIAO_ESP32C5)
  static const int SERIAL1_TX_PIN = 6;   // D4 on XIAO ESP32-C5
  static const int SERIAL1_RX_PIN = 7;   // D5 on XIAO ESP32-C5
#else
  static const int SERIAL1_TX_PIN = 5;   // D4 on XIAO ESP32-S3
  static const int SERIAL1_RX_PIN = 6;   // D5 on XIAO ESP32-S3
#endif

// Change per device for multi-node deduplication in mesh-mapper.py
#define NODE_ID  "RX01"

// Both reads within a dwell window must exceed RSSI_THRESHOLD to report.
// Raising MIN_DWELL_HITS reduces false positives at the cost of sensitivity.
#define MIN_DWELL_HITS  2

// Minimum ms between repeat reports for the same channel
#define REPORT_INTERVAL_MS  5000UL

// ============================================================
// State
// ============================================================

static unsigned long last_heartbeat_ms               = 0;
static unsigned long last_report_ms[FPV_CHANNEL_COUNT] = {};

// ============================================================
// Helpers
// ============================================================

// Stable synthetic MAC: encodes frequency + band + channel.
// mesh-mapper.py tracks each FPV channel as a distinct "device".
//   AF:00 = locally-administered Analog FM prefix
//   [freq_hi][freq_lo] = frequency in MHz, big-endian
//   [band_ascii][ch_num]
static void channel_to_mac(const FPVChannel& chan, char* out_mac) {
    snprintf(out_mac, 18, "AF:00:%02X:%02X:%02X:%02X",
             (chan.freq_mhz >> 8) & 0xFF,
              chan.freq_mhz       & 0xFF,
             (uint8_t)chan.band[0],
              chan.ch);
}

static void emit_detection(int idx, int rssi_raw) {
    const FPVChannel& chan = FPV_CHANNELS[idx];

    char mac[18];
    channel_to_mac(chan, mac);

    char basic_id[24];
    snprintf(basic_id, sizeof(basic_id), "5.8G-%s%d-%uMHz",
             chan.band, chan.ch, chan.freq_mhz);

    StaticJsonDocument<256> doc;
    doc["type"]     = "analog_fm";
    doc["mac"]      = mac;
    doc["freq_mhz"] = chan.freq_mhz;
    doc["band"]     = chan.band;
    doc["ch"]       = chan.ch;
    doc["rssi_raw"] = rssi_raw;
    doc["rssi"]     = rssi_raw;  // mesh-mapper.py reads "rssi" for display
    doc["basic_id"] = basic_id;
    doc["node_id"]  = NODE_ID;

    serializeJson(doc, Serial);
    Serial.println();

#if ENABLE_MESH_RELAY
    char relay[80];
    snprintf(relay, sizeof(relay), "AnalogFM: %s%d %uMHz rssi=%d [%s]",
             chan.band, chan.ch, chan.freq_mhz, rssi_raw, NODE_ID);
    if (Serial1.availableForWrite() >= (int)strlen(relay) + 2)
        Serial1.println(relay);
#endif
}

static void emit_heartbeat() {
    StaticJsonDocument<128> doc;
    doc["heartbeat"] = true;
    doc["node_id"]   = NODE_ID;
    doc["scanning"]  = true;
    doc["channels"]  = FPV_CHANNEL_COUNT;
    doc["threshold"] = RSSI_THRESHOLD;
    serializeJson(doc, Serial);
    Serial.println();
}

// ============================================================
// Arduino entry points
// ============================================================

void setup() {
    Serial.begin(115200);
    // Wait up to 3 s for USB-CDC host connection before emitting data
    while (!Serial && millis() < 3000) {}

#if ENABLE_MESH_RELAY
    Serial1.begin(115200, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
#endif

    rx5808_init();

    Serial.printf(
        "{\"info\":\"RX5808 scanner ready\",\"node_id\":\"%s\","
        "\"channels\":%d,\"threshold\":%d}\n",
        NODE_ID, FPV_CHANNEL_COUNT, RSSI_THRESHOLD
    );
}

void loop() {
    unsigned long now = millis();

    // Periodic heartbeat — filtered by mesh-mapper.py's heartbeat handler
    if (now - last_heartbeat_ms >= 60000UL) {
        emit_heartbeat();
        last_heartbeat_ms = now;
    }

    // Sequential channel scan
    for (int i = 0; i < FPV_CHANNEL_COUNT; i++) {
        rx5808_set_frequency(FPV_CHANNELS[i].freq_mhz);
        delay(TUNE_SETTLE_MS);

        // Take MIN_DWELL_HITS readings; all must be above threshold.
        // Accumulate the sum so we can report the average, not just the last sample.
        int  hits     = 0;
        long rssi_sum = 0;
        for (int j = 0; j < MIN_DWELL_HITS; j++) {
            int r = rx5808_read_rssi();
            if (r >= RSSI_THRESHOLD) {
                hits++;
                rssi_sum += r;
            }
            delay(5);
        }

        if (hits >= MIN_DWELL_HITS) {
            if (now - last_report_ms[i] >= REPORT_INTERVAL_MS) {
                emit_detection(i, (int)(rssi_sum / hits));
                last_report_ms[i] = now;
            }
        }
    }
}
