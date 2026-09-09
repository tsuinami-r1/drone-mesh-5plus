/*
 * Level 1 station v2 — analog FPV detection with sector direction finding
 * and video fingerprinting
 * Supports: XIAO ESP32-S3  |  XIAO ESP32-C5
 *
 * Sweeps every channel of the selected receiver (RX5808: 40 × 5.8 GHz). At
 * each channel the SP4T switch presents all four sector patches in turn and
 * RSSI is read on each, so a hit carries a calibrated power per sector and a
 * bearing. The strongest hits are then fine-tuned to their carrier centre and
 * held while the sync separator's line and field pulses are counted, which
 * says whether the carrier is really analog video, PAL or NTSC, and yields a
 * per-drone fingerprint.
 *
 * Output
 *   USB Serial  — full JSON lines consumed by mesh-mapper.py
 *   Serial1 UART — compact JSON lines (≤ MESH_LINE_MAX) for the Heltec /
 *                  Meshtastic relay; the home node forwards JSON unchanged
 *
 * Pins and every tuneable: config.h
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include "soc/soc_caps.h"
#include "config.h"
#include "analog_receiver.h"
#include "sector_switch.h"
#include "bearing.h"
#include "video_sync.h"

// ============================================================
// State
// ============================================================

static unsigned long last_heartbeat_ms                = 0;
static unsigned long last_mesh_heartbeat_ms           = 0;
static unsigned long last_report_ms[RX_CHANNEL_COUNT] = {};
static unsigned long last_mesh_ms[RX_CHANNEL_COUNT]   = {};
static uint16_t      report_seq                       = 0;   // wraps; enough to spot mesh loss
static uint16_t      video_seen_count                 = 0;   // carriers with a sync train since boot

// Per-sweep measurements
struct ChannelScan {
    RssiStats sector[SECTOR_COUNT];   // one dwell read per sector
    RssiStats best;                   // merged dwell reads on the strongest sector
    float     sector_dbm[SECTOR_COUNT];
    uint8_t   best_sector;
    bool      hit;
};
static ChannelScan scan[RX_CHANNEL_COUNT];

// Extra measurements taken on the strongest peaks after the sweep
struct PeakExtra {
    bool        valid;
    uint16_t    freq_peak;
    VideoResult video;
};

// ============================================================
// Helpers
// ============================================================

// Stable synthetic MAC: encodes frequency + band + channel.
//   AF:00 = locally-administered Analog FM prefix
//   [freq_hi][freq_lo] = frequency in MHz, big-endian
//   [band_ascii][ch_num]
static void channel_to_mac(const AnalogChannel& chan, char* out_mac) {
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
// message and a separated line ending can even go out on its own.
static void mesh_write_line(const char* line, int len) {
    if (len > MESH_LINE_MAX) len = MESH_LINE_MAX;
    if (Serial1.availableForWrite() >= len + 1) {
        Serial1.write((const uint8_t*)line, len);
        Serial1.write((uint8_t)'\n');
    }
}
#endif

static void usb_write_line(const char* line, int len) {
    Serial.write((const uint8_t*)line, len);
    Serial.write((uint8_t)'\n');
}

// ============================================================
// Reporting
// ============================================================

static void emit_detection(int idx, const PeakExtra& extra, bool to_usb, bool to_mesh) {
    const AnalogChannel& chan = RX_CHANNELS[idx];
    const ChannelScan&   cs   = scan[idx];

    char mac[18];
    channel_to_mac(chan, mac);

    const int   raw = cs.best.mean_raw();
    const int   mv  = cs.best.mean_mv();
    const float dbm = rx_mv_to_dbm(mv);
    const float thr = rx_raw_to_dbm(RSSI_THRESHOLD);
    const Bearing b = bearing_estimate(cs.sector_dbm, thr);
    report_seq++;

    // Fingerprint: <standard>/<line_hz>/<carrier MHz>; only when video was measured
    char fp[32] = "";
    if (extra.valid && extra.video.present) {
        snprintf(fp, sizeof(fp), "%s/%u/%u",
                 extra.video.standard, extra.video.line_hz, extra.freq_peak);
    }

    if (to_usb) {
        char line[512];
        int len = snprintf(line, sizeof(line),
            "{\"type\":\"analog_fm\",\"receiver\":\"" RX_NAME "\",\"hw\":\"" STATION_HW "\","
            "\"mac\":\"%s\",\"freq_mhz\":%u,\"band\":\"%s\",\"ch\":%u,"
            "\"rssi_raw\":%d,\"rssi\":%d,\"rssi_mv\":%d,\"rssi_dbm\":%.1f,"
            "\"rssi_min\":%d,\"rssi_max\":%d,\"rssi_n\":%d,"
            "\"sectors\":[%.1f,%.1f,%.1f,%.1f],\"sector\":%u,"
            "\"bearing_deg\":%d,\"bearing_sigma_deg\":%u",
            mac, chan.freq_mhz, chan.band, chan.ch,
            raw, raw, mv, dbm,
            cs.best.min_raw, cs.best.max_raw, cs.best.n,
            cs.sector_dbm[0], cs.sector_dbm[1], cs.sector_dbm[2], cs.sector_dbm[3], cs.best_sector,
            b.deg, b.sigma_deg);
        if (extra.valid) {
            len += snprintf(line + len, sizeof(line) - len,
                ",\"freq_peak\":%u,\"video\":\"%s\",\"sync_hz\":%u,\"field_hz\":%u,\"sync_q\":%u",
                extra.freq_peak, extra.video.standard, extra.video.line_hz,
                extra.video.field_hz, extra.video.quality);
            if (fp[0]) len += snprintf(line + len, sizeof(line) - len, ",\"fp\":\"%s\"", fp);
        }
        len += snprintf(line + len, sizeof(line) - len,
            ",\"basic_id\":\"" RX_BAND_LABEL "-%s%u-%uMHz\",\"node_id\":\"" NODE_ID "\",\"seq\":%u}",
            chan.band, chan.ch, chan.freq_mhz, (unsigned)report_seq);
        if (len >= (int)sizeof(line)) len = sizeof(line) - 1;
        usb_write_line(line, len);
    }

#if ENABLE_MESH_RELAY
    if (to_mesh) {
        // Compact copy for the LoRa hop. Keeps what the position solver and
        // the fingerprint need; the mapper backfills rssi/basic_id/receiver.
        // Optional groups are dropped from the tail if the line would exceed
        // MESH_LINE_MAX, so the relay never emits truncated JSON:
        //   level 2: bearing + video/fp     level 1: bearing only     level 0: bare
        char relay[MESH_LINE_MAX + 64];
        int  rlen = 0;
        for (int level = 2; level >= 0; level--) {
            rlen = snprintf(relay, sizeof(relay),
                "{\"type\":\"analog_fm\",\"mac\":\"%s\",\"freq_mhz\":%u,\"band\":\"%s\",\"ch\":%u,"
                "\"rssi_dbm\":%.1f",
                mac, chan.freq_mhz, chan.band, chan.ch, dbm);
            if (level >= 1) {
                rlen += snprintf(relay + rlen, sizeof(relay) - rlen,
                    ",\"bearing_deg\":%d,\"bearing_sigma_deg\":%u", b.deg, b.sigma_deg);
            }
            if (level >= 2 && extra.valid) {
                // fp already names the standard; "video":"none" only when measured and absent
                if (fp[0]) rlen += snprintf(relay + rlen, sizeof(relay) - rlen, ",\"fp\":\"%s\"", fp);
                else       rlen += snprintf(relay + rlen, sizeof(relay) - rlen, ",\"video\":\"none\"");
            }
            rlen += snprintf(relay + rlen, sizeof(relay) - rlen,
                ",\"node_id\":\"" NODE_ID "\",\"seq\":%u}", (unsigned)report_seq);
            if (rlen <= MESH_LINE_MAX) break;
        }
        if (rlen > MESH_LINE_MAX) rlen = MESH_LINE_MAX;   // cannot happen with NODE_ID ≤ 12 chars
        mesh_write_line(relay, rlen);
    }
#else
    (void)to_mesh;
#endif
}

static void emit_heartbeat(bool to_usb, bool to_mesh) {
    const float thr_dbm = rx_raw_to_dbm(RSSI_THRESHOLD);
    const unsigned up_s = (unsigned)(millis() / 1000UL);
    char line[256];
    int  len;

    if (to_usb) {
        len = snprintf(line, sizeof(line),
            "{\"heartbeat\":true,\"node_id\":\"" NODE_ID "\",\"receiver\":\"" RX_NAME "\",\"hw\":\"" STATION_HW "\","
            "\"scanning\":true,\"channels\":%d,\"sectors\":%d,\"heading\":%d,"
            "\"threshold\":%d,\"threshold_dbm\":%.1f,\"video_seen\":%u,"
            "\"temp_c\":%.1f,\"uptime_s\":%u,\"seq\":%u}",
            RX_CHANNEL_COUNT, SECTOR_COUNT, STATION_HEADING_DEG,
            RSSI_THRESHOLD, thr_dbm, (unsigned)video_seen_count,
            board_temp_c(), up_s, (unsigned)report_seq);
        if (len >= (int)sizeof(line)) len = sizeof(line) - 1;
        usb_write_line(line, len);
    }
#if ENABLE_MESH_RELAY
    if (to_mesh) {
        // Mesh copy carries only what the mapper stores per station
        len = snprintf(line, sizeof(line),
            "{\"heartbeat\":true,\"node_id\":\"" NODE_ID "\",\"receiver\":\"" RX_NAME "\",\"hw\":\"" STATION_HW "\","
            "\"heading\":%d,\"threshold_dbm\":%.1f,\"video_seen\":%u,\"temp_c\":%.1f,\"uptime_s\":%u,\"seq\":%u}",
            STATION_HEADING_DEG, thr_dbm, (unsigned)video_seen_count, board_temp_c(), up_s, (unsigned)report_seq);
        if (len >= (int)sizeof(line)) len = sizeof(line) - 1;
        mesh_write_line(line, len);
    }
#else
    (void)to_mesh;
#endif
}

// ============================================================
// Measurement
// ============================================================

// Visit one channel: one dwell read on every sector, then confirm the
// strongest sector with MIN_DWELL_HITS reads that must all clear the threshold.
static void scan_channel(int idx) {
    ChannelScan& cs = scan[idx];
    rx_tune(RX_CHANNELS[idx].freq_mhz);
    delay(RX_TUNE_SETTLE_MS);

    cs.best_sector = 0;
    for (uint8_t s = 0; s < SECTOR_COUNT; s++) {
        sector_select(s);
        rx_read_rssi_stats(&cs.sector[s]);
        cs.sector_dbm[s] = rx_mv_to_dbm(cs.sector[s].mean_mv());
        if (cs.sector[s].mean_mv() > cs.sector[cs.best_sector].mean_mv()) cs.best_sector = s;
    }

    sector_select(cs.best_sector);
    cs.best = cs.sector[cs.best_sector];
    int hits = (cs.best.mean_raw() >= RSSI_THRESHOLD) ? 1 : 0;
    for (int j = 1; j < MIN_DWELL_HITS; j++) {
        delay(5);
        RssiStats dwell;
        rx_read_rssi_stats(&dwell);
        if (dwell.mean_raw() >= RSSI_THRESHOLD) hits++;
        cs.best.merge(dwell);
    }
    cs.hit = (hits >= MIN_DWELL_HITS);
    if (cs.hit) cs.sector_dbm[cs.best_sector] = rx_mv_to_dbm(cs.best.mean_mv());
}

// True if channel i is the strongest hit within ±PEAK_WINDOW_MHZ. Ties go to
// the lower index, which also folds the duplicate 5880 MHz (R7 / F8) into one.
static bool is_local_peak(int i) {
    const int fi = RX_CHANNELS[i].freq_mhz;
    const int ri = scan[i].best.mean_mv();
    for (int j = 0; j < RX_CHANNEL_COUNT; j++) {
        if (j == i || !scan[j].hit) continue;
        int df = RX_CHANNELS[j].freq_mhz - fi;
        if (df < -PEAK_WINDOW_MHZ || df > PEAK_WINDOW_MHZ) continue;
        int rj = scan[j].best.mean_mv();
        if (rj > ri || (rj == ri && j < i)) return false;
    }
    return true;
}

// Step the synthesizer across ±FINE_TUNE_SPAN_MHZ around the hit and return
// the frequency with the strongest RSSI: the carrier centre, which is stable
// per VTX and part of the fingerprint.
static uint16_t fine_tune_peak(int idx) {
    const uint16_t f0 = RX_CHANNELS[idx].freq_mhz;
#if FINE_TUNE_ENABLE
    sector_select(scan[idx].best_sector);
    uint16_t best_f  = f0;
    int      best_mv = -1;
    for (int off = -FINE_TUNE_SPAN_MHZ; off <= FINE_TUNE_SPAN_MHZ; off += FINE_TUNE_STEP_MHZ) {
        uint16_t f = (uint16_t)(f0 + off);
        rx_tune(f);
        delay(RX_TUNE_SETTLE_MS);
        RssiStats st;
        rx_read_rssi_stats(&st);
        if (st.mean_mv() > best_mv) { best_mv = st.mean_mv(); best_f = f; }
    }
    return best_f;
#else
    return f0;
#endif
}

// Fine-tune, then hold the receiver on the carrier centre and count sync.
static PeakExtra measure_peak(int idx) {
    PeakExtra e;
    e.valid     = true;
    e.freq_peak = fine_tune_peak(idx);
    rx_tune(e.freq_peak);
    delay(RX_TUNE_SETTLE_MS);
    sector_select(scan[idx].best_sector);
    e.video = video_measure();
    if (e.video.present) video_seen_count++;
    return e;
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
    pinMode(PIN_MESH_TX, OUTPUT);
    digitalWrite(PIN_MESH_TX, HIGH);
#endif

    Serial.begin(115200);
    // Wait up to 3 s for USB-CDC host connection before emitting data
    while (!Serial && millis() < 3000) {}

#if ENABLE_MESH_RELAY
    // TX ring buffer so availableForWrite() reflects real headroom (default
    // FIFO-only depth is ~128 B); must be set before begin().
    Serial1.setTxBufferSize(512);
    Serial1.begin(115200, SERIAL_8N1, PIN_MESH_RX, PIN_MESH_TX);
#endif

    rx_init();
    sector_init();
    video_init();

    // Pre-age the per-channel report timers so a transmitter already active at
    // boot is reported on the first sweep instead of being suppressed for the
    // first interval (millis() starts near 0 -> now - 0 < interval).
    for (int i = 0; i < RX_CHANNEL_COUNT; i++) {
        last_report_ms[i] = (unsigned long)0 - REPORT_INTERVAL_MS;
        last_mesh_ms[i]   = (unsigned long)0 - MESH_REPORT_INTERVAL_MS;
    }
    last_mesh_heartbeat_ms = (unsigned long)0 - MESH_HEARTBEAT_INTERVAL_MS;

    Serial.printf(
        "{\"info\":\"" RX_NAME " " STATION_HW " station ready\",\"node_id\":\"" NODE_ID "\","
        "\"receiver\":\"" RX_NAME "\",\"hw\":\"" STATION_HW "\",\"channels\":%d,\"sectors\":%d,"
        "\"heading\":%d,\"threshold\":%d,\"peak_pick\":%d,\"video\":%d,\"fine_tune\":%d}\n",
        RX_CHANNEL_COUNT, SECTOR_COUNT, STATION_HEADING_DEG, RSSI_THRESHOLD,
        PEAK_PICK, VIDEO_ENABLE, FINE_TUNE_ENABLE
    );
}

void loop() {
    unsigned long now = millis();

    // Heartbeat: the mapper records the station as alive with its threshold,
    // which turns silence about an emitter into a "not near here" constraint.
    bool hb_usb  = (now - last_heartbeat_ms      >= HEARTBEAT_INTERVAL_MS);
    bool hb_mesh = (now - last_mesh_heartbeat_ms >= MESH_HEARTBEAT_INTERVAL_MS);
    if (hb_usb || hb_mesh) {
        emit_heartbeat(hb_usb, hb_mesh);
        if (hb_usb)  last_heartbeat_ms      = now;
        if (hb_mesh) last_mesh_heartbeat_ms = now;
    }

    // ---- 1. Sweep: every channel, every sector ----
    for (int i = 0; i < RX_CHANNEL_COUNT; i++) scan_channel(i);

    // ---- 2. Decide what to report ----
    now = millis();
    int  due[RX_CHANNEL_COUNT];
    bool due_usb[RX_CHANNEL_COUNT], due_mesh[RX_CHANNEL_COUNT];
    int  n_due = 0;
    for (int i = 0; i < RX_CHANNEL_COUNT; i++) {
        if (!scan[i].hit) continue;
#if PEAK_PICK
        if (!is_local_peak(i)) continue;
#endif
        bool u = (now - last_report_ms[i] >= REPORT_INTERVAL_MS);
        bool m = (now - last_mesh_ms[i]   >= MESH_REPORT_INTERVAL_MS);
        if (!(u || m)) continue;
        due[n_due] = i; due_usb[n_due] = u; due_mesh[n_due] = m; n_due++;
    }

    // ---- 3. Fine-tune + video on the strongest due peaks ----
    // Sort the due list strongest-first (small list, insertion sort).
    for (int a = 1; a < n_due; a++) {
        int  di = due[a]; bool du = due_usb[a], dm = due_mesh[a];
        int  b  = a - 1;
        while (b >= 0 && scan[due[b]].best.mean_mv() < scan[di].best.mean_mv()) {
            due[b + 1] = due[b]; due_usb[b + 1] = due_usb[b]; due_mesh[b + 1] = due_mesh[b]; b--;
        }
        due[b + 1] = di; due_usb[b + 1] = du; due_mesh[b + 1] = dm;
    }

    for (int k = 0; k < n_due; k++) {
        int i = due[k];
        PeakExtra extra;
        extra.valid = false;
        if (k < VIDEO_MAX_PER_SWEEP) extra = measure_peak(i);
        emit_detection(i, extra, due_usb[k], due_mesh[k]);
        unsigned long t = millis();
        if (due_usb[k])  last_report_ms[i] = t;
        if (due_mesh[k]) last_mesh_ms[i]   = t;
    }
}
