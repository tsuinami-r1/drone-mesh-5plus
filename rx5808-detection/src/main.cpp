/*
 * RX5808 5.8GHz Analog FM Detection — XIAO ESP32-S3
 *
 * Scans all 40 standard FPV channels (Raceband + Bands A/B/E/F) and emits a
 * JSON detection on USB Serial whenever RSSI exceeds the configured threshold,
 * indicating a nearby 5.8GHz analog FM video transmitter (FPV drone).
 *
 * Output
 *   USB Serial  — JSON lines consumed by mesh-mapper.py
 *   Serial1 UART (TX=GPIO5, RX=GPIO6) — compact text for Heltec/Meshtastic relay
 *                                        (set ENABLE_MESH_RELAY 0 to disable)
 *
 * Wiring (XIAO ESP32-S3)
 *   GPIO9  (D10) → RX5808 DATA
 *   GPIO7  (D8)  → RX5808 CLK
 *   GPIO8  (D9)  → RX5808 CS
 *   GPIO1  (D0)  ← RX5808 RSSI  (analog, 0–3.3V)
 *   3.3V         → RX5808 VCC
 *   GND          → RX5808 GND
 *
 *   GPIO5  (D4)  → Heltec RX  (optional mesh relay)
 *   GPIO6  (D5)  ← Heltec TX  (optional mesh relay)
 *
 * Detection JSON example
 *   {"type":"analog_fm","mac":"AF:00:16:1A:52:01","freq_mhz":5658,
 *    "band":"R","ch":1,"rssi_raw":2240,"rssi":2240,
 *    "basic_id":"5.8G-R1-5658MHz","node_id":"RX01"}
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include "rx5808.h"

// ============================================================
// Configuration
// ============================================================

// Set to 0 if you are not wiring a Heltec LoRa V3 for mesh relay
#define ENABLE_MESH_RELAY  1

// UART pins for Heltec relay (same as all other firmware variants)
static const int SERIAL1_TX_PIN = 5;
static const int SERIAL1_RX_PIN = 6;

// Per-device identifier used for multi-node deduplication in mesh-mapper.py
#define NODE_ID  "RX01"

// Number of consecutive RSSI reads that must exceed the threshold before
// a detection is reported. Raising this reduces burst false positives.
#define MIN_DWELL_HITS  2

// Minimum milliseconds between repeat reports for the same channel.
// Prevents flooding the serial port when a strong signal is parked on one freq.
#define REPORT_INTERVAL_MS  5000UL

// ============================================================
// State
// ============================================================

static unsigned long last_heartbeat_ms = 0;
static unsigned long last_report_ms[40] = {};  // one entry per channel index

// ============================================================
// Helpers
// ============================================================

// Build a stable synthetic MAC from frequency + band + channel so that
// mesh-mapper.py can track each FPV channel as a distinct "device".
//   byte 0: 0xAF  (locally-administered, Analog FM marker)
//   byte 1: 0x00
//   bytes 2-3: frequency in MHz, big-endian
//   byte 4: ASCII band letter
//   byte 5: channel number
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

    // basic_id carries human-readable channel info visible in mesh-mapper.py
    char basic_id[24];
    snprintf(basic_id, sizeof(basic_id), "5.8G-%s%d-%uMHz",
             chan.band, chan.ch, chan.freq_mhz);

    StaticJsonDocument<256> doc;
    doc["type"]      = "analog_fm";
    doc["mac"]       = mac;
    doc["freq_mhz"]  = chan.freq_mhz;
    doc["band"]      = chan.band;
    doc["ch"]        = chan.ch;
    doc["rssi_raw"]  = rssi_raw;
    doc["rssi"]      = rssi_raw;   // mesh-mapper.py reads this field for RSSI display
    doc["basic_id"]  = basic_id;
    doc["node_id"]   = NODE_ID;

    serializeJson(doc, Serial);
    Serial.println();

#if ENABLE_MESH_RELAY
    // Keep the relay message under Meshtastic's 230-byte payload limit
    char relay[128];
    snprintf(relay, sizeof(relay),
             "AnalogFM: %s%d %uMHz rssi=%d [%s]",
             chan.band, chan.ch, chan.freq_mhz, rssi_raw, NODE_ID);
    if (Serial1.availableForWrite() >= (int)strlen(relay) + 2)
        Serial1.println(relay);
#endif
}

static void emit_heartbeat() {
    StaticJsonDocument<128> doc;
    doc["heartbeat"]  = true;
    doc["node_id"]    = NODE_ID;
    doc["scanning"]   = true;
    doc["channels"]   = FPV_CHANNEL_COUNT;
    doc["threshold"]  = RSSI_THRESHOLD;
    serializeJson(doc, Serial);
    Serial.println();
}

// ============================================================
// Arduino entry points
// ============================================================

void setup() {
    Serial.begin(115200);
    // Give the host a moment to open the port before emitting anything
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

    // Periodic heartbeat (skipped by mesh-mapper.py's heartbeat filter)
    if (now - last_heartbeat_ms >= 60000UL) {
        emit_heartbeat();
        last_heartbeat_ms = now;
    }

    // Sequential channel scan
    for (int i = 0; i < FPV_CHANNEL_COUNT; i++) {
        rx5808_set_frequency(FPV_CHANNELS[i].freq_mhz);
        delay(TUNE_SETTLE_MS);

        // Require MIN_DWELL_HITS consecutive reads above threshold
        int hits = 0;
        int rssi  = 0;
        for (int j = 0; j < MIN_DWELL_HITS; j++) {
            rssi = rx5808_read_rssi();
            if (rssi >= RSSI_THRESHOLD) hits++;
            delay(5);
        }

        if (hits >= MIN_DWELL_HITS) {
            // Rate-limit repeated reports for the same channel
            if (now - last_report_ms[i] >= REPORT_INTERVAL_MS) {
                emit_detection(i, rssi);
                last_report_ms[i] = now;
            }
        }
    }
}
