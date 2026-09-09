/*
 * RX5808 5.8GHz Analog FM Detection
 * Supports: XIAO ESP32-S3  |  XIAO ESP32-C5
 *
 * Scans all 40 standard FPV channels (Raceband + Bands A/B/E/F) and emits a
 * JSON detection whenever RSSI exceeds the configured threshold, indicating
 * a nearby 5.8GHz analog FM video transmitter (FPV drone).
 *
 * Every detection carries a calibrated received power ("rssi_dbm") and the
 * spread of the samples behind it, so mesh-mapper.py can fuse the same
 * emitter across several stations (differential-RSSI multilateration)
 * instead of drawing one ring per station.
 *
 * Output
 *   USB Serial  — full JSON lines consumed by mesh-mapper.py
 *   Serial1 UART — compact JSON lines for the Heltec/Meshtastic relay; the
 *                  home node forwards JSON to the mapper unchanged and tags
 *                  anything else "[MESH]", which the mapper drops
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
#include "soc/soc_caps.h"
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

// Change per device for multi-node deduplication in mesh-mapper.py.
// Must equal the paired Meshtastic node's shortName/longName.
#define NODE_ID  "RX01"

// Receiver name carried in every line ("receiver" key)
#define RECEIVER_NAME  "rx5808"

// Every dwell read within a channel visit must exceed RSSI_THRESHOLD to report.
// Raising MIN_DWELL_HITS reduces false positives at the cost of sensitivity.
#define MIN_DWELL_HITS  2

// Minimum ms between repeat USB reports for the same channel
#define REPORT_INTERVAL_MS       5000UL

// Minimum ms between repeat mesh reports for the same channel. LoRa airtime
// is the scarce resource in a dense deployment: at 10 s, six stations
// watching one VTX put ~0.6 msg/s on the mesh.
#define MESH_REPORT_INTERVAL_MS  10000UL

// Heartbeat cadence. The mesh copy lets the mapper know a station is alive
// and what its threshold is, which is what turns a silent station into a
// "the emitter is NOT near here" constraint for the position solver.
#define HEARTBEAT_INTERVAL_MS       60000UL
#define MESH_HEARTBEAT_INTERVAL_MS 120000UL

// Peak-pick: an analog video carrier is ~20 MHz wide and lights every
// channel within ±PEAK_WINDOW_MHZ of the true frequency. With PEAK_PICK 1
// only the strongest channel of each such cluster is reported, so one VTX
// is one tracking key per station (and one mesh message instead of 3–6).
// Set 0 to report every channel above threshold, as older firmware did.
#define PEAK_PICK        1
#define PEAK_WINDOW_MHZ  20

// ============================================================
// State
// ============================================================

static unsigned long last_heartbeat_ms                 = 0;
static unsigned long last_mesh_heartbeat_ms            = 0;
static unsigned long last_report_ms[FPV_CHANNEL_COUNT] = {};
static unsigned long last_mesh_ms[FPV_CHANNEL_COUNT]   = {};
static uint32_t      report_seq                        = 0;

// Per-sweep results
static RssiStats sweep_stats[FPV_CHANNEL_COUNT];
static bool      sweep_hit[FPV_CHANNEL_COUNT];

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

static float board_temp_c() {
#if SOC_TEMP_SENSOR_SUPPORTED
    return temperatureRead();
#else
    return -273.0f;
#endif
}

#if ENABLE_MESH_RELAY
// Single burst, '\n' only (no CR): the Meshtastic serial module in TEXTMSG
// mode broadcasts raw unframed chunks, so a CR rides along inside the mesh
// message and a separated line ending can even go out on its own as a
// blank-rendering message.
static void mesh_write_line(const char* line, int len) {
    if (Serial1.availableForWrite() >= len + 1) {
        Serial1.write((const uint8_t*)line, len);
        Serial1.write((uint8_t)'\n');
    }
}
#endif

static void emit_detection(int idx, const RssiStats& st, bool to_usb, bool to_mesh) {
    const FPVChannel& chan = FPV_CHANNELS[idx];

    char mac[18];
    channel_to_mac(chan, mac);

    const int   raw = st.mean_raw();
    const int   mv  = st.mean_mv();
    const float dbm = rx5808_mv_to_dbm(mv);
    report_seq++;

    if (to_usb) {
        // Full line. Keys the mapper has always read come first; the
        // additive keys (receiver, rssi_mv, rssi_dbm, rssi_min/max/n, seq)
        // are what the multi-station fusion uses.
        char line[320];
        int  len = snprintf(line, sizeof(line),
            "{\"type\":\"analog_fm\",\"receiver\":\"" RECEIVER_NAME "\","
            "\"mac\":\"%s\",\"freq_mhz\":%u,\"band\":\"%s\",\"ch\":%u,"
            "\"rssi_raw\":%d,\"rssi\":%d,\"rssi_mv\":%d,\"rssi_dbm\":%.1f,"
            "\"rssi_min\":%d,\"rssi_max\":%d,\"rssi_n\":%d,"
            "\"basic_id\":\"5.8G-%s%u-%uMHz\",\"node_id\":\"" NODE_ID "\",\"seq\":%lu}",
            mac, chan.freq_mhz, chan.band, chan.ch,
            raw, raw, mv, dbm,
            st.min_raw, st.max_raw, st.n,
            chan.band, chan.ch, chan.freq_mhz, (unsigned long)report_seq);
        if (len >= (int)sizeof(line)) len = sizeof(line) - 1;
        Serial.write((const uint8_t*)line, len);
        Serial.write((uint8_t)'\n');
    }

#if ENABLE_MESH_RELAY
    if (to_mesh) {
        // Compact copy for the LoRa hop: must stay well under the ~200-byte
        // Meshtastic text payload. Drops "rssi"/"basic_id"/"receiver"/"rssi_mv"/
        // "rssi_n", which the mapper backfills for analog_fm lines.
        char relay[200];
        int  rlen = snprintf(relay, sizeof(relay),
            "{\"type\":\"analog_fm\",\"mac\":\"%s\",\"freq_mhz\":%u,\"band\":\"%s\",\"ch\":%u,"
            "\"rssi_raw\":%d,\"rssi_dbm\":%.1f,\"rssi_min\":%d,\"rssi_max\":%d,"
            "\"node_id\":\"" NODE_ID "\",\"seq\":%lu}",
            mac, chan.freq_mhz, chan.band, chan.ch,
            raw, dbm, st.min_raw, st.max_raw, (unsigned long)report_seq);
        if (rlen >= (int)sizeof(relay)) rlen = sizeof(relay) - 1;
        mesh_write_line(relay, rlen);
    }
#else
    (void)to_mesh;
#endif
}

static void emit_heartbeat(bool to_usb, bool to_mesh) {
    const float thr_dbm = rx5808_raw_to_dbm(RSSI_THRESHOLD);
    char line[224];
    int  len = snprintf(line, sizeof(line),
        "{\"heartbeat\":true,\"node_id\":\"" NODE_ID "\",\"receiver\":\"" RECEIVER_NAME "\","
        "\"scanning\":true,\"channels\":%d,\"threshold\":%d,\"threshold_dbm\":%.1f,"
        "\"temp_c\":%.1f,\"uptime_s\":%lu,\"seq\":%lu}",
        FPV_CHANNEL_COUNT, RSSI_THRESHOLD, thr_dbm,
        board_temp_c(), (unsigned long)(millis() / 1000UL), (unsigned long)report_seq);
    if (len >= (int)sizeof(line)) len = sizeof(line) - 1;

    if (to_usb) {
        Serial.write((const uint8_t*)line, len);
        Serial.write((uint8_t)'\n');
    }
#if ENABLE_MESH_RELAY
    if (to_mesh) mesh_write_line(line, len);
#else
    (void)to_mesh;
#endif
}

// True if channel i is the strongest hit within ±PEAK_WINDOW_MHZ. Ties go to
// the lower index, which also folds the duplicate 5880 MHz (R7 / F8) into one.
static bool is_local_peak(int i) {
    const int fi = FPV_CHANNELS[i].freq_mhz;
    const int ri = sweep_stats[i].mean_raw();
    for (int j = 0; j < FPV_CHANNEL_COUNT; j++) {
        if (j == i || !sweep_hit[j]) continue;
        int df = FPV_CHANNELS[j].freq_mhz - fi;
        if (df < -PEAK_WINDOW_MHZ || df > PEAK_WINDOW_MHZ) continue;
        int rj = sweep_stats[j].mean_raw();
        if (rj > ri || (rj == ri && j < i)) return false;
    }
    return true;
}

// ============================================================
// Arduino entry points
// ============================================================

void setup() {
#if ENABLE_MESH_RELAY
    // Park the mesh UART TX line idle-high before the USB-CDC wait: a
    // floating TX feeds noise into the Meshtastic radio's serial RX, which
    // TEXTMSG mode broadcasts as blank/garbage mesh messages on every
    // reboot or brownout.
    pinMode(SERIAL1_TX_PIN, OUTPUT);
    digitalWrite(SERIAL1_TX_PIN, HIGH);
#endif

    Serial.begin(115200);
    // Wait up to 3 s for USB-CDC host connection before emitting data
    while (!Serial && millis() < 3000) {}

#if ENABLE_MESH_RELAY
    // TX ring buffer so availableForWrite() reflects real headroom (default
    // FIFO-only depth is ~128 B); must be set before begin().
    Serial1.setTxBufferSize(512);
    Serial1.begin(115200, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
#endif

    rx5808_init();

    // Pre-age the per-channel report timers so a transmitter already active at
    // boot is reported on the first sweep instead of being suppressed for the
    // first interval (millis() starts near 0 -> now - 0 < interval).
    for (int i = 0; i < FPV_CHANNEL_COUNT; i++) {
        last_report_ms[i] = (unsigned long)0 - REPORT_INTERVAL_MS;
        last_mesh_ms[i]   = (unsigned long)0 - MESH_REPORT_INTERVAL_MS;
    }
    last_mesh_heartbeat_ms = (unsigned long)0 - MESH_HEARTBEAT_INTERVAL_MS;

    Serial.printf(
        "{\"info\":\"" RECEIVER_NAME " scanner ready\",\"node_id\":\"" NODE_ID "\","
        "\"receiver\":\"" RECEIVER_NAME "\",\"channels\":%d,\"threshold\":%d,"
        "\"peak_pick\":%d}\n",
        FPV_CHANNEL_COUNT, RSSI_THRESHOLD, PEAK_PICK
    );
}

void loop() {
    unsigned long now = millis();

    // Periodic heartbeat — filtered by mesh-mapper.py's heartbeat handler,
    // which records the station as alive for the coverage-overlap solver.
    bool hb_usb  = (now - last_heartbeat_ms      >= HEARTBEAT_INTERVAL_MS);
    bool hb_mesh = (now - last_mesh_heartbeat_ms >= MESH_HEARTBEAT_INTERVAL_MS);
    if (hb_usb || hb_mesh) {
        emit_heartbeat(hb_usb, hb_mesh);
        if (hb_usb)  last_heartbeat_ms      = now;
        if (hb_mesh) last_mesh_heartbeat_ms = now;
    }

    // ---- Sequential channel sweep: measure everything first ----
    for (int i = 0; i < FPV_CHANNEL_COUNT; i++) {
        rx5808_set_frequency(FPV_CHANNELS[i].freq_mhz);
        delay(TUNE_SETTLE_MS);

        // MIN_DWELL_HITS dwell reads; every one must clear the threshold.
        // All samples are merged so the report carries mean/min/max/n over
        // the whole visit, not just the last read.
        RssiStats acc;
        acc.reset();
        int hits = 0;
        for (int j = 0; j < MIN_DWELL_HITS; j++) {
            RssiStats dwell;
            rx5808_read_rssi_stats(&dwell);
            if (dwell.mean_raw() >= RSSI_THRESHOLD) hits++;
            acc.merge(dwell);
            delay(5);
        }
        sweep_stats[i] = acc;
        sweep_hit[i]   = (hits >= MIN_DWELL_HITS);
    }

    // ---- Then report: peak channels only (or all hits with PEAK_PICK 0) ----
    now = millis();
    for (int i = 0; i < FPV_CHANNEL_COUNT; i++) {
        if (!sweep_hit[i]) continue;
#if PEAK_PICK
        if (!is_local_peak(i)) continue;
#endif
        bool usb_due  = (now - last_report_ms[i] >= REPORT_INTERVAL_MS);
        bool mesh_due = (now - last_mesh_ms[i]   >= MESH_REPORT_INTERVAL_MS);
        if (usb_due || mesh_due) {
            emit_detection(i, sweep_stats[i], usb_due, mesh_due);
            if (usb_due)  last_report_ms[i] = now;
            if (mesh_due) last_mesh_ms[i]   = now;
        }
    }
}
