/*
 * Level 1 station v3 prototype — analog FPV detection on the ESP32-C5's own
 * 5 GHz Wi-Fi PHY, with sector direction finding and a software video check.
 * Supports: XIAO ESP32-C5 only.
 *
 * Replaces the v2 station's RX5808 (tuning + RSSI) and sync separator (video
 * check) with the C5 itself:
 *
 *   tuning      the Wi-Fi PHY is parked on the nearest public 5 GHz centre and
 *               retuned to the exact FPV frequency            (c5phy_rf.*)
 *   RSSI        raw I/Q from the modem's diagnostic bus, read back through
 *               PARLIO; power at a fixed, known gain -> dBm   (iq_capture.*, demod.*)
 *   video check software FM demodulation of the same I/Q and a search for
 *               horizontal sync repeating at the PAL/NTSC line period (demod.*)
 *
 * Sweeps every channel; at each channel the SP4T switch presents the four
 * sector patches in turn and each sector's power and FM coherence are
 * measured, so a hit carries a calibrated power per sector and a bearing. The
 * strongest hits are then held while several I/Q windows are demodulated for
 * the video check and fingerprint.
 *
 * Output
 *   USB Serial   full JSON lines consumed by mesh-mapper.py (same contract as v2)
 *   Serial1 UART compact JSON lines (<= MESH_LINE_MAX) for the Heltec /
 *                Meshtastic relay on D4/D5, as every other station tier
 *
 * Pins and every tuneable: config.h
 *
 * The PHY front end, capture path and metrics derive from C5VRX
 * (github.com/colonelpanichacks/c5vrx), GPL-3.0-only. See LICENSE.
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <math.h>
#include <string.h>
#include "soc/soc_caps.h"
#include "config.h"
#include "c5phy_rf.h"
#include "iq_capture.h"
#include "demod.h"
#include "sector_switch.h"
#include "bearing.h"

// ============================================================
// Channel table
// ============================================================

struct AnalogChannel {
    uint16_t    freq_mhz;
    const char* band;     // "R","A","B","E","F" (+ "L" with SCAN_LOW_BAND)
    uint8_t     ch;       // 1–8
};

// Standard FPV bands — Raceband, A, B, E, F. R7 and F8 are both 5880 MHz.
static const AnalogChannel RX_CHANNELS[] = {
    // Raceband
    {5658, "R", 1}, {5695, "R", 2}, {5732, "R", 3}, {5769, "R", 4},
    {5806, "R", 5}, {5843, "R", 6}, {5880, "R", 7}, {5917, "R", 8},
    // Band A
    {5865, "A", 1}, {5845, "A", 2}, {5825, "A", 3}, {5805, "A", 4},
    {5785, "A", 5}, {5765, "A", 6}, {5745, "A", 7}, {5725, "A", 8},
    // Band B
    {5733, "B", 1}, {5752, "B", 2}, {5771, "B", 3}, {5790, "B", 4},
    {5809, "B", 5}, {5828, "B", 6}, {5847, "B", 7}, {5866, "B", 8},
    // Band E
    {5705, "E", 1}, {5685, "E", 2}, {5665, "E", 3}, {5645, "E", 4},
    {5885, "E", 5}, {5905, "E", 6}, {5925, "E", 7}, {5945, "E", 8},
    // Band F (Fatshark)
    {5740, "F", 1}, {5760, "F", 2}, {5780, "F", 3}, {5800, "F", 4},
    {5820, "F", 5}, {5840, "F", 6}, {5860, "F", 7}, {5880, "F", 8},
#if SCAN_LOW_BAND
    // LowBand / Band D
    {5362, "L", 1}, {5399, "L", 2}, {5436, "L", 3}, {5473, "L", 4},
    {5510, "L", 5}, {5547, "L", 6}, {5584, "L", 7}, {5621, "L", 8},
#endif
};
static_assert(sizeof(RX_CHANNELS) / sizeof(RX_CHANNELS[0]) == RX_CHANNEL_COUNT,
              "RX_CHANNEL_COUNT does not match RX_CHANNELS array length");
static_assert(WINDOWS_PER_SECTOR >= 1 && WINDOWS_PER_SECTOR <= 8, "WINDOWS_PER_SECTOR must be 1..8");
static_assert(WINDOWS_PER_SECTOR_QUICK >= 1 && WINDOWS_PER_SECTOR_QUICK <= WINDOWS_PER_SECTOR,
              "WINDOWS_PER_SECTOR_QUICK must be 1..WINDOWS_PER_SECTOR");
static_assert(VIDEO_WINDOWS >= 1 && VIDEO_WINDOWS <= 32, "VIDEO_WINDOWS must be 1..32");
static_assert(VIDEO_MIN_WINDOWS >= 1 && VIDEO_MIN_WINDOWS <= VIDEO_WINDOWS, "VIDEO_MIN_WINDOWS must be 1..VIDEO_WINDOWS");

// ============================================================
// State
// ============================================================

static bool          g_rf_ok                         = false;
static unsigned long last_heartbeat_ms               = 0;
static unsigned long last_mesh_heartbeat_ms          = 0;
static unsigned long last_report_ms[RX_CHANNEL_COUNT] = {};
static unsigned long last_mesh_ms[RX_CHANNEL_COUNT]   = {};
static uint16_t      report_seq                      = 0;   // wraps; enough to spot mesh loss
static uint16_t      video_seen_count                = 0;   // carriers with a sync train since boot
static uint32_t      sweep_count                     = 0;
static uint16_t      tune_fail_count                 = 0;   // channels the regulatory table refused

// One sector's measurement on one channel
struct SectorMeasure {
    float   level_db;      // min over windows, dB above the noise reference
    float   level_max_db;  // max over windows (spread)
    float   dbm;           // calibrated
    int     q_phase;       // median over windows
    int     cfo_khz;
    bool    cfo_valid;
    int     clip_permille; // worst window kept
    uint8_t gain;          // gain index the level was measured at
    bool    noise_like;    // majority of windows looked like Gaussian noise
    bool    modulated;     // any window showed FM phase swing
    uint8_t windows;       // windows that went into the numbers
};

struct ChannelScan {
    SectorMeasure sector[SECTOR_COUNT];
    float         sector_dbm[SECTOR_COUNT];
    uint8_t       best_sector;
    bool          hit;
    bool          tunable;   // false once the regulatory table refused it
    uint8_t       misses;    // consecutive sweeps without a hit (quick dwell)
};
static ChannelScan scan[RX_CHANNEL_COUNT];

// Extra measurements taken on the strongest peaks after the sweep
struct PeakExtra {
    bool        valid;
    uint16_t    freq_peak;      // tuned frequency + measured carrier offset
    int         cfo_khz;
    bool        present;        // video found on >= VIDEO_MIN_WINDOWS windows
    bool        pal;
    const char* standard;       // "PAL", "NTSC", "none"
    uint16_t    line_hz;
    uint8_t     field_hz;       // nominal for the standard (not measured on v3)
    uint8_t     sync_q;         // % of windows with a sync train
    uint8_t     sync_score;     // mean sync score of those windows
    uint8_t     windows;
};

// Bench console
#if BENCH_CONSOLE
static bool    bench_hold      = false;
static int     bench_channel   = 0;
static uint8_t bench_sector    = 0;
static int     bench_gain      = -1;      // -1 = automatic
static char    bench_line[48];
static uint8_t bench_len       = 0;
#endif

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

// dB above the noise reference -> dBm (RSSI_CAL_* in config.h)
static float level_to_dbm(float level_db) {
    float dbm = RSSI_CAL_DBM_AT_NOISE + level_db * RSSI_CAL_SLOPE + RSSI_CAL_OFFSET_DB;
    if (dbm < -120.0f) dbm = -120.0f;
    if (dbm >    0.0f) dbm =    0.0f;
    return dbm;
}

// The mapper contract requires rssi_raw as an RX5808-style ADC count and
// only ever converts it back with its own RX5808 curve (0–1320 counts ~=
// -95..-20 dBm) when rssi_dbm is missing. Invert that curve so a mapper
// without rssi_dbm support still draws the right ring.
static int dbm_to_pseudo_raw(float dbm) {
    float mv  = 450.0f + (dbm + 95.0f) * (650.0f / 75.0f);
    float raw = mv / (3100.0f / 4095.0f);
    if (raw < 0) raw = 0;
    if (raw > 4095) raw = 4095;
    return (int)lroundf(raw);
}

// Change the forced gain and give the PHY time to settle only if it changed
static void set_gain_settled(uint8_t g) {
    if (g < RF_GAIN_MIN) g = RF_GAIN_MIN;
    if (g > RF_GAIN_MAX) g = RF_GAIN_MAX;
    if (rf_gain() == g) return;
    rf_set_gain(g);
    delay(RF_GAIN_SETTLE_MS);
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

static void status_led(bool on) {
    digitalWrite(PIN_STATUS_LED, on ? LOW : HIGH);   // active LOW
}

// ============================================================
// Measurement
// ============================================================

// One sector on the current channel: 'windows' I/Q windows at the highest
// gain that does not clip. Level = minimum over windows (a Wi-Fi burst lifts
// one window, a continuous carrier lifts them all), coherence = median.
static bool measure_sector(int windows, SectorMeasure* out) {
    memset(out, 0, sizeof(*out));
    if (windows > WINDOWS_PER_SECTOR) windows = WINDOWS_PER_SECTOR;

    set_gain_settled(RF_GAIN_MAX);

    float lvl[WINDOWS_PER_SECTOR];
    int   q[WINDOWS_PER_SECTOR];
    int   got = 0, attempts = 0, failures = 0;
    long  cfo_sum = 0; int cfo_n = 0;
    int   noise_votes = 0, mod_votes = 0, clip_max = 0;

    while (got < windows && attempts < windows + 8) {
        attempts++;
        const uint8_t* buf;
        if (!iq_capture(&buf)) {
            if (++failures >= 3) break;
            continue;
        }
        IqMetrics m;
        iq_metrics(buf, IQ_WINDOW_BYTES, &m);

        // Clipping 4-bit I/Q: step the gain down and take the window again
        if (m.clip_permille > RF_CLIP_MAX_PERMILLE && rf_gain() > RF_GAIN_MIN) {
            set_gain_settled((uint8_t)(rf_gain() > RF_GAIN_STEP_DOWN + RF_GAIN_MIN
                                       ? rf_gain() - RF_GAIN_STEP_DOWN : RF_GAIN_MIN));
            continue;
        }

        float p = m.p_mean < 0.05f ? 0.05f : m.p_mean;
        lvl[got] = (float)(RF_GAIN_MAX - rf_gain()) * RF_GAIN_DB_PER_STEP
                 + 10.0f * log10f(p / RF_NOISE_POWER);
        q[got]   = m.q_phase;
        if (m.cfo_valid) { cfo_sum += m.cfo_khz; cfo_n++; }
        if (m.noise_like) noise_votes++;
        if (m.modulated)  mod_votes++;
        if (m.clip_permille > clip_max) clip_max = m.clip_permille;
        got++;
    }
    if (got == 0) return false;

    float lmin = lvl[0], lmax = lvl[0];
    for (int k = 1; k < got; k++) {
        if (lvl[k] < lmin) lmin = lvl[k];
        if (lvl[k] > lmax) lmax = lvl[k];
    }
    // median of a tiny array: insertion sort
    for (int a = 1; a < got; a++) {
        int x = q[a], b = a - 1;
        while (b >= 0 && q[b] > x) { q[b + 1] = q[b]; b--; }
        q[b + 1] = x;
    }

    out->level_db      = lmin;
    out->level_max_db  = lmax;
    out->dbm           = level_to_dbm(lmin);
    out->q_phase       = q[got / 2];
    out->cfo_valid     = cfo_n > 0;
    out->cfo_khz       = cfo_n ? (int)(cfo_sum / cfo_n) : 0;
    out->clip_permille = clip_max;
    out->gain          = rf_gain();
    out->noise_like    = noise_votes * 2 > got;
    out->modulated     = mod_votes > 0;
    out->windows       = (uint8_t)got;
    return true;
}

// Visit one channel: every sector, then decide.
static void scan_channel(int idx) {
    ChannelScan& cs = scan[idx];
    cs.hit = false;
    if (!cs.tunable) return;

    if (!rf_tune(RX_CHANNELS[idx].freq_mhz)) {
        cs.tunable = false;
        tune_fail_count++;
        return;
    }
    delay(RF_TUNE_SETTLE_MS);

    const bool quick = (cs.misses >= QUICK_DWELL_MISSES) &&
                       (sweep_count % QUICK_DWELL_RECHECK_SWEEPS) != 0;
    const int windows = quick ? WINDOWS_PER_SECTOR_QUICK : WINDOWS_PER_SECTOR;

    cs.best_sector = 0;
    for (uint8_t s = 0; s < SECTOR_COUNT; s++) {
        sector_select(s);
        if (!measure_sector(windows, &cs.sector[s])) {
            cs.sector[s].level_db = -30.0f;           // capture failed: floor it
            cs.sector[s].dbm      = level_to_dbm(-30.0f);
        }
        cs.sector_dbm[s] = cs.sector[s].dbm;
        if (cs.sector[s].level_db > cs.sector[cs.best_sector].level_db) cs.best_sector = s;
    }

    const SectorMeasure& b = cs.sector[cs.best_sector];
    cs.hit = (b.windows > 0) &&
             (b.level_db >= DETECT_LEVEL_DB) &&
             (b.q_phase  >= DETECT_Q_PHASE_MIN);
    if (cs.hit) cs.misses = 0;
    else if (cs.misses < 255) cs.misses++;
}

// True if channel i is the strongest hit within ±PEAK_WINDOW_MHZ. Ties go to
// the lower index, which also folds the duplicate 5880 MHz (R7 / F8) into one.
static bool is_local_peak(int i) {
    const int   fi = RX_CHANNELS[i].freq_mhz;
    const float li = scan[i].sector[scan[i].best_sector].level_db;
    for (int j = 0; j < RX_CHANNEL_COUNT; j++) {
        if (j == i || !scan[j].hit) continue;
        int df = RX_CHANNELS[j].freq_mhz - fi;
        if (df < -PEAK_WINDOW_MHZ || df > PEAK_WINDOW_MHZ) continue;
        float lj = scan[j].sector[scan[j].best_sector].level_db;
        if (lj > li || (lj == li && j < i)) return false;
    }
    return true;
}

// Hold the receiver on the hit (strongest sector, its measured gain) and
// demodulate VIDEO_WINDOWS windows for the video check; the carrier offset
// of the same windows gives the fingerprint's frequency.
static PeakExtra measure_peak(int idx) {
    PeakExtra e;
    memset(&e, 0, sizeof(e));
    e.standard = "none";

    const AnalogChannel& chan = RX_CHANNELS[idx];
    ChannelScan&         cs   = scan[idx];
    const SectorMeasure& best = cs.sector[cs.best_sector];

    if (!rf_tune(chan.freq_mhz)) return e;
    delay(RF_TUNE_SETTLE_MS);
    sector_select(cs.best_sector);
    set_gain_settled(best.gain);

    int   n_video = 0, pal_votes = 0, ntsc_votes = 0, q_sum = 0, T_n = 0;
    float T_sum = 0;
    long  cfo_sum = 0; int cfo_n = 0;
    int   attempts = 0, done = 0;

    while (done < VIDEO_WINDOWS && attempts < VIDEO_WINDOWS + 6) {
        attempts++;
        const uint8_t* buf;
        if (!iq_capture(&buf)) continue;

        IqMetrics m;
        iq_metrics(buf, IQ_WINDOW_BYTES, &m);
        if (m.clip_permille > RF_CLIP_MAX_PERMILLE && rf_gain() > RF_GAIN_MIN) {
            set_gain_settled((uint8_t)(rf_gain() > RF_GAIN_STEP_DOWN + RF_GAIN_MIN
                                       ? rf_gain() - RF_GAIN_STEP_DOWN : RF_GAIN_MIN));
            continue;
        }
        if (m.cfo_valid) { cfo_sum += m.cfo_khz; cfo_n++; }

#if VIDEO_ENABLE
        VideoWindow v;
        video_window(buf, IQ_WINDOW_BYTES, &v);
        if (v.video) {
            n_video++;
            if (v.pal) pal_votes++; else ntsc_votes++;
            T_sum += v.period_samples;
            T_n++;
            q_sum += v.quality;
        }
#endif
        done++;
        if (done < VIDEO_WINDOWS) delay(VIDEO_WINDOW_GAP_MS);
    }

    e.valid     = done > 0;
    e.windows   = (uint8_t)done;
    e.cfo_khz   = cfo_n ? (int)(cfo_sum / cfo_n) : best.cfo_khz;
    e.freq_peak = (uint16_t)lroundf((float)chan.freq_mhz + (float)e.cfo_khz / 1000.0f);
    e.sync_q    = done ? (uint8_t)((n_video * 100) / done) : 0;
    e.sync_score = n_video ? (uint8_t)(q_sum / n_video) : 0;
    e.present   = (n_video >= VIDEO_MIN_WINDOWS) && (T_n > 0);
    if (e.present) {
        e.pal      = pal_votes >= ntsc_votes;
        e.standard = e.pal ? "PAL" : "NTSC";
        e.line_hz  = (uint16_t)lroundf((float)IQ_SAMPLE_RATE_HZ / (T_sum / (float)T_n));
        e.field_hz = e.pal ? 50 : 60;
        video_seen_count++;
    }
    return e;
}

// ============================================================
// Reporting
// ============================================================

static void emit_detection(int idx, const PeakExtra& extra, bool to_usb, bool to_mesh) {
    const AnalogChannel& chan = RX_CHANNELS[idx];
    const ChannelScan&   cs   = scan[idx];
    const SectorMeasure& best = cs.sector[cs.best_sector];

    char mac[18];
    channel_to_mac(chan, mac);

    const float   dbm = best.dbm;
    const float   thr = level_to_dbm(DETECT_LEVEL_DB);
    const Bearing b   = bearing_estimate(cs.sector_dbm, thr);
    report_seq++;

    // Fingerprint: <standard>/<line_hz>/<carrier MHz>; only when video was found
    char fp[32] = "";
    if (extra.valid && extra.present) {
        snprintf(fp, sizeof(fp), "%s/%u/%u", extra.standard, extra.line_hz, extra.freq_peak);
    }

    if (to_usb) {
        char line[640];
        int len = snprintf(line, sizeof(line),
            "{\"type\":\"analog_fm\",\"receiver\":\"" RX_NAME "\",\"hw\":\"" STATION_HW "\","
            "\"mac\":\"%s\",\"freq_mhz\":%u,\"band\":\"%s\",\"ch\":%u,"
            "\"rssi_raw\":%d,\"rssi\":%d,\"rssi_dbm\":%.1f,"
            "\"rssi_min\":%d,\"rssi_max\":%d,\"rssi_n\":%u,"
            "\"level_db\":%.1f,\"gain\":%u,\"q_phase\":%d,\"cfo_khz\":%d,\"carrier\":\"%s\","
            "\"sectors\":[%.1f,%.1f,%.1f,%.1f],\"sector\":%u,"
            "\"bearing_deg\":%d,\"bearing_sigma_deg\":%u",
            mac, chan.freq_mhz, chan.band, chan.ch,
            dbm_to_pseudo_raw(dbm), dbm_to_pseudo_raw(dbm), dbm,
            dbm_to_pseudo_raw(level_to_dbm(best.level_db)),
            dbm_to_pseudo_raw(level_to_dbm(best.level_max_db)),
            (unsigned)best.windows,
            best.level_db, (unsigned)best.gain, best.q_phase, best.cfo_khz,
            best.noise_like ? "noise" : (best.modulated ? "fm" : "cw"),
            cs.sector_dbm[0], cs.sector_dbm[1], cs.sector_dbm[2], cs.sector_dbm[3], cs.best_sector,
            b.deg, b.sigma_deg);
        if (extra.valid) {
            len += snprintf(line + len, sizeof(line) - len,
                ",\"freq_peak\":%u,\"video\":\"%s\",\"sync_hz\":%u,\"field_hz\":%u,"
                "\"sync_q\":%u,\"sync_score\":%u,\"video_windows\":%u",
                extra.freq_peak, extra.standard, extra.line_hz, extra.field_hz,
                extra.sync_q, extra.sync_score, extra.windows);
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
        // Compact copy for the LoRa hop: identical to the v2 station's line.
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
    const float    thr_dbm = level_to_dbm(DETECT_LEVEL_DB);
    const unsigned up_s    = (unsigned)(millis() / 1000UL);
    int  nf = 0;
    bool nf_ok = g_rf_ok && rf_phy_noise_dbm(&nf);
    char line[384];
    int  len;

    if (to_usb) {
        len = snprintf(line, sizeof(line),
            "{\"heartbeat\":true,\"node_id\":\"" NODE_ID "\",\"receiver\":\"" RX_NAME "\",\"hw\":\"" STATION_HW "\","
            "\"scanning\":%s,\"channels\":%d,\"sectors\":%d,\"heading\":%d,"
            "\"threshold\":%d,\"threshold_dbm\":%.1f,\"video_seen\":%u,"
            "\"gain_max\":%d,\"bw40\":%d,\"tune_fail\":%u,\"cap_err\":%lu,\"sweeps\":%lu,",
            g_rf_ok ? "true" : "false",
            RX_CHANNEL_COUNT - (int)tune_fail_count, SECTOR_COUNT, STATION_HEADING_DEG,
            dbm_to_pseudo_raw(thr_dbm), thr_dbm, (unsigned)video_seen_count,
            RF_GAIN_MAX, RF_BW40, (unsigned)tune_fail_count,
            (unsigned long)iq_error_count(), (unsigned long)sweep_count);
        if (nf_ok) len += snprintf(line + len, sizeof(line) - len, "\"nf_dbm\":%d,", nf);
        len += snprintf(line + len, sizeof(line) - len,
            "\"temp_c\":%.1f,\"uptime_s\":%u,\"seq\":%u}",
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
// Bench console (USB). Lines, '\n' or '\r' terminated:
//   ?            help + status
//   h R3 | h 5732   hold this channel and stream per-window metrics
//   s 0..3       select sector while holding
//   g 2..62 | g a   fixed gain / automatic while holding
//   v            video check on the held channel (VIDEO_WINDOWS windows)
//   x            resume scanning
// Every line it prints carries "info", which mesh-mapper.py drops as a
// non-detection, so the console is safe to leave enabled on a fielded
// station. Unknown input is ignored.
// ============================================================
#if BENCH_CONSOLE

static int find_channel(const char* s) {
    while (*s == ' ') s++;
    if (!*s) return -1;
    if (isdigit((unsigned char)s[0])) {
        int f = atoi(s);
        for (int i = 0; i < RX_CHANNEL_COUNT; i++) if (RX_CHANNELS[i].freq_mhz == f) return i;
        return -1;
    }
    char band = (char)toupper((unsigned char)s[0]);
    int  ch   = atoi(s + 1);
    for (int i = 0; i < RX_CHANNEL_COUNT; i++)
        if (RX_CHANNELS[i].band[0] == band && RX_CHANNELS[i].ch == ch) return i;
    return -1;
}

static void bench_help() {
    Serial.println("{\"info\":\"bench help\",\"cmds\":\"? | h R3 | h 5732 | s 0-3 | g 2-62 | g a | v | x\"}");
    Serial.printf("{\"info\":\"bench status\",\"rf\":%s,\"hold\":%s,\"freq_mhz\":%u,\"wifi_ch\":%u,"
                  "\"sector\":%u,\"gain\":%u,\"bw40\":%d,\"tune_fail\":%u,\"captures\":%lu,\"cap_err\":%lu,"
                  "\"sweeps\":%lu,\"err\":\"%s\"}\n",
                  g_rf_ok ? "true" : "false", bench_hold ? "true" : "false",
                  rf_status().tuned_mhz, rf_status().wifi_channel, sector_current(), rf_gain(),
                  rf_status().bw40 ? 1 : 0, (unsigned)tune_fail_count,
                  (unsigned long)iq_capture_count(), (unsigned long)iq_error_count(),
                  (unsigned long)sweep_count, rf_last_error());
}

static void bench_video() {
    if (!g_rf_ok) return;
    int n_video = 0, pal = 0, ntsc = 0;
    for (int w = 0; w < VIDEO_WINDOWS; w++) {
        const uint8_t* buf;
        if (!iq_capture(&buf)) continue;
        VideoWindow v;
        video_window(buf, IQ_WINDOW_BYTES, &v);
        Serial.printf("{\"info\":\"bench_video\",\"w\":%d,\"video\":%d,\"std\":\"%s\",\"period\":%.1f,"
                      "\"line_hz\":%d,\"quality\":%d,\"periods\":%d,\"pulses\":%d,\"swing\":%d}\n",
                      w, v.video ? 1 : 0, v.n_periods ? (v.pal ? "PAL" : "NTSC") : "-",
                      v.period_samples,
                      v.period_samples > 0 ? (int)lroundf((float)IQ_SAMPLE_RATE_HZ / v.period_samples) : 0,
                      v.quality, v.n_periods, v.n_pulses, v.swing);
        if (v.video) { n_video++; if (v.pal) pal++; else ntsc++; }
        delay(VIDEO_WINDOW_GAP_MS);
    }
    Serial.printf("{\"info\":\"bench_video_sum\",\"windows\":%d,\"video\":%d,\"pal\":%d,\"ntsc\":%d,\"present\":%d}\n",
                  VIDEO_WINDOWS, n_video, pal, ntsc, (n_video >= VIDEO_MIN_WINDOWS) ? 1 : 0);
}

static void bench_command(const char* cmd) {
    while (*cmd == ' ') cmd++;
    switch (cmd[0]) {
    case '?': bench_help(); break;
    case 'x': case 'X':
        bench_hold = false;
        bench_gain = -1;
        Serial.println("{\"info\":\"bench resume\"}");
        break;
    case 'h': case 'H': {
        int idx = find_channel(cmd + 1);
        if (idx < 0) { Serial.println("{\"info\":\"bench error\",\"error\":\"unknown channel\"}"); break; }
        if (!g_rf_ok) { Serial.println("{\"info\":\"bench error\",\"error\":\"rf not started\"}"); break; }
        if (!rf_tune(RX_CHANNELS[idx].freq_mhz)) {
            Serial.println("{\"info\":\"bench error\",\"error\":\"tune refused by regulatory table\"}");
            break;
        }
        delay(RF_TUNE_SETTLE_MS);
        bench_hold    = true;
        bench_channel = idx;
        sector_select(bench_sector);
        Serial.printf("{\"info\":\"bench hold\",\"freq_mhz\":%u,\"band\":\"%s\",\"ch\":%u,\"wifi_ch\":%u}\n",
                      RX_CHANNELS[idx].freq_mhz, RX_CHANNELS[idx].band, RX_CHANNELS[idx].ch,
                      rf_status().wifi_channel);
        break;
    }
    case 's': case 'S': {
        int s = atoi(cmd + 1);
        if (s < 0 || s >= SECTOR_COUNT) s = 0;
        bench_sector = (uint8_t)s;
        sector_select(bench_sector);
        Serial.printf("{\"info\":\"bench sector\",\"sector\":%u}\n", bench_sector);
        break;
    }
    case 'g': case 'G': {
        const char* a = cmd + 1;
        while (*a == ' ') a++;
        if (*a == 'a' || *a == 'A') { bench_gain = -1; Serial.println("{\"info\":\"bench gain auto\"}"); break; }
        int g = atoi(a);
        if (g < RF_GAIN_MIN) g = RF_GAIN_MIN;
        if (g > RF_GAIN_MAX) g = RF_GAIN_MAX;
        bench_gain = g;
        set_gain_settled((uint8_t)g);
        Serial.printf("{\"info\":\"bench gain\",\"gain\":%d}\n", g);
        break;
    }
    case 'v': case 'V':
        if (!bench_hold) { Serial.println("{\"info\":\"bench error\",\"error\":\"hold a channel first (h R3)\"}"); break; }
        bench_video();
        break;
    default:
        // Anything else is ignored silently. mesh-mapper.py writes
        // "WATCHDOG_RESET" to every station it opens over USB; answering it
        // with help text would only clutter the mapper's log.
        break;
    }
}

static void bench_poll_console() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (bench_len) { bench_line[bench_len] = 0; bench_command(bench_line); }
            bench_len = 0;
        } else if (bench_len < sizeof(bench_line) - 1) {
            bench_line[bench_len++] = c;
        }
    }
}

// One metrics line per call while holding a channel
static void bench_tick() {
    if (bench_gain < 0) set_gain_settled(RF_GAIN_MAX);
    const uint8_t* buf;
    IqMetrics m;
    int tries = 0;
    for (;;) {
        if (!iq_capture(&buf)) { Serial.println("{\"info\":\"bench\",\"error\":\"capture failed\"}"); return; }
        iq_metrics(buf, IQ_WINDOW_BYTES, &m);
        if (bench_gain < 0 && m.clip_permille > RF_CLIP_MAX_PERMILLE && rf_gain() > RF_GAIN_MIN && tries++ < 6) {
            set_gain_settled((uint8_t)(rf_gain() > RF_GAIN_STEP_DOWN + RF_GAIN_MIN
                                       ? rf_gain() - RF_GAIN_STEP_DOWN : RF_GAIN_MIN));
            continue;
        }
        break;
    }
    float p = m.p_mean < 0.05f ? 0.05f : m.p_mean;
    float level_db = (float)(RF_GAIN_MAX - rf_gain()) * RF_GAIN_DB_PER_STEP + 10.0f * log10f(p / RF_NOISE_POWER);
    int phy_rssi = 0, nf = 0;
    bool r_ok = rf_phy_rssi_dbm(&phy_rssi), n_ok = rf_phy_noise_dbm(&nf);
    Serial.printf("{\"info\":\"bench\",\"freq_mhz\":%u,\"sector\":%u,\"gain\":%u,\"level_db\":%.1f,\"rssi_dbm\":%.1f,"
                  "\"q_phase\":%d,\"p_mean\":%.2f,\"p_med\":%d,\"clip\":%d,\"origin\":%d,\"cfo_khz\":%d,"
                  "\"mod\":%d,\"noise\":%d,\"phy_rssi\":%d,\"nf_dbm\":%d}\n",
                  RX_CHANNELS[bench_channel].freq_mhz, sector_current(), rf_gain(), level_db, level_to_dbm(level_db),
                  m.q_phase, m.p_mean, m.p_median, m.clip_permille, m.origin_permille, m.cfo_khz,
                  m.modulated ? 1 : 0, m.noise_like ? 1 : 0, r_ok ? phy_rssi : 0, n_ok ? nf : 0);
}
#endif // BENCH_CONSOLE

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
    pinMode(PIN_STATUS_LED, OUTPUT);
    status_led(false);

    Serial.begin(115200);
    // Wait up to 3 s for USB-CDC host connection before emitting data
    while (!Serial && millis() < 3000) {}

#if ENABLE_MESH_RELAY
    // TX ring buffer so availableForWrite() reflects real headroom (default
    // FIFO-only depth is ~128 B); must be set before begin().
    Serial1.setTxBufferSize(512);
    Serial1.begin(115200, SERIAL_8N1, PIN_MESH_RX, PIN_MESH_TX);
#endif

    sector_init();
    demod_init();

    // Front end: PHY first (routes the I/Q lanes), then the PARLIO reader,
    // then the lane routing once more because the PARLIO driver reconfigures
    // those GPIOs as inputs.
    g_rf_ok = rf_start();
    if (g_rf_ok) {
        if (!iq_init()) {
            g_rf_ok = false;
            Serial.println("{\"info\":\"error\",\"node_id\":\"" NODE_ID "\",\"error\":\"PARLIO RX init failed\"}");
        } else {
            rf_route_iq_lanes();
        }
    } else {
        Serial.printf("{\"info\":\"error\",\"node_id\":\"" NODE_ID "\",\"error\":\"rf_start: %s\"}\n", rf_last_error());
    }

    for (int i = 0; i < RX_CHANNEL_COUNT; i++) {
        scan[i].tunable = true;
        scan[i].misses  = 0;
        // Pre-age the per-channel report timers so a transmitter already
        // active at boot is reported on the first sweep instead of being
        // suppressed for the first interval (millis() starts near 0).
        last_report_ms[i] = (unsigned long)0 - REPORT_INTERVAL_MS;
        last_mesh_ms[i]   = (unsigned long)0 - MESH_REPORT_INTERVAL_MS;
    }
    last_mesh_heartbeat_ms = (unsigned long)0 - MESH_HEARTBEAT_INTERVAL_MS;

    Serial.printf(
        "{\"info\":\"" RX_NAME " " STATION_HW " station ready\",\"node_id\":\"" NODE_ID "\","
        "\"receiver\":\"" RX_NAME "\",\"hw\":\"" STATION_HW "\",\"channels\":%d,\"sectors\":%d,"
        "\"heading\":%d,\"threshold_dbm\":%.1f,\"threshold_level_db\":%.1f,\"q_min\":%d,"
        "\"peak_pick\":%d,\"video\":%d,\"bw40\":%d,\"gain_max\":%d,\"window_us\":%u,\"rf\":%s}\n",
        RX_CHANNEL_COUNT, SECTOR_COUNT, STATION_HEADING_DEG,
        level_to_dbm(DETECT_LEVEL_DB), (double)DETECT_LEVEL_DB, DETECT_Q_PHASE_MIN,
        PEAK_PICK, VIDEO_ENABLE, RF_BW40, RF_GAIN_MAX,
        (unsigned)(IQ_WINDOW_BYTES * 1000000ULL / IQ_SAMPLE_RATE_HZ),
        g_rf_ok ? "true" : "false"
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

#if BENCH_CONSOLE
    bench_poll_console();
    if (bench_hold) {
        status_led(true);
        bench_tick();
        delay(200);
        return;
    }
#endif

    if (!g_rf_ok) {
        // Nothing to scan; keep heartbeating so the mapper shows the station as
        // alive-but-not-scanning, and blink the LED as a visible fault.
        status_led((now / 250) & 1);
        delay(100);
        return;
    }

    // A refused channel is normally the regulatory table, which is permanent,
    // but give every channel another chance now and then.
    if ((sweep_count % 32) == 0) {
        for (int i = 0; i < RX_CHANNEL_COUNT; i++) scan[i].tunable = true;
        tune_fail_count = 0;
    }

    // ---- 1. Sweep: every channel, every sector ----
    for (int i = 0; i < RX_CHANNEL_COUNT; i++) {
        scan_channel(i);
#if BENCH_CONSOLE
        bench_poll_console();
        if (bench_hold) return;
#endif
    }
    sweep_count++;

    // ---- 2. Decide what to report ----
    now = millis();
    int  due[RX_CHANNEL_COUNT];
    bool due_usb[RX_CHANNEL_COUNT], due_mesh[RX_CHANNEL_COUNT];
    int  n_due = 0, n_hits = 0;
    for (int i = 0; i < RX_CHANNEL_COUNT; i++) {
        if (!scan[i].hit) continue;
        n_hits++;
#if PEAK_PICK
        if (!is_local_peak(i)) continue;
#endif
        bool u = (now - last_report_ms[i] >= REPORT_INTERVAL_MS);
        bool m = (now - last_mesh_ms[i]   >= MESH_REPORT_INTERVAL_MS);
        if (!(u || m)) continue;
        due[n_due] = i; due_usb[n_due] = u; due_mesh[n_due] = m; n_due++;
    }
    status_led(n_hits > 0);

    // ---- 3. Video check on the strongest due peaks ----
    // Sort the due list strongest-first (small list, insertion sort).
    for (int a = 1; a < n_due; a++) {
        int  di = due[a]; bool du = due_usb[a], dm = due_mesh[a];
        int  b  = a - 1;
        while (b >= 0 && scan[due[b]].sector[scan[due[b]].best_sector].level_db
                       < scan[di].sector[scan[di].best_sector].level_db) {
            due[b + 1] = due[b]; due_usb[b + 1] = due_usb[b]; due_mesh[b + 1] = due_mesh[b]; b--;
        }
        due[b + 1] = di; due_usb[b + 1] = du; due_mesh[b + 1] = dm;
    }

    for (int k = 0; k < n_due; k++) {
        int i = due[k];
        PeakExtra extra;
        memset(&extra, 0, sizeof(extra));
        extra.standard = "none";
        if (k < VIDEO_MAX_PER_SWEEP) extra = measure_peak(i);
        emit_detection(i, extra, due_usb[k], due_mesh[k]);
        unsigned long t = millis();
        if (due_usb[k])  last_report_ms[i] = t;
        if (due_mesh[k]) last_mesh_ms[i]   = t;
    }
}
