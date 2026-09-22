#pragma once
#include <Arduino.h>

// ============================================================
// Level 1 station v3 prototype — configuration
//
// The receiver is the XIAO ESP32-C5's own 5 GHz Wi-Fi PHY, tuned onto the
// analog FPV channels and tapped for raw I/Q (see c5phy_rf.*). There is no
// RX5808 and no sync separator. Every tuneable and every pin lives here.
// ============================================================

// ---- Station identity ----
// Change per device. Must equal the paired Meshtastic node's shortName /
// longName, or the mapper draws this station's output in the wrong place.
// Keep it <= 6 characters: every extra character costs one byte of mesh line.
#define NODE_ID          "RX01"

// Hardware revision reported in every heartbeat ("hw")
#define STATION_HW       "v3"

// Receiver name reported in every line ("receiver"). The mapper labels it
// "5.8G" by frequency; no mapper change is needed.
#define RX_NAME          "c5phy"
#define RX_BAND_LABEL    "5.8G"

// True bearing (degrees) of the enclosure face carrying patch N. Bearings are
// reported relative to true north, so this must be measured at install time.
#define STATION_HEADING_DEG   0

// ============================================================
// Board guard — XIAO ESP32-C5 only
// ============================================================
// The whole receiver is the C5's 5 GHz PHY, so unlike the v2 station this
// firmware cannot be interchangeable with the S3.
#if !defined(ARDUINO_XIAO_ESP32C5) || !defined(CONFIG_IDF_TARGET_ESP32C5)
  #error "Level 1 v3 (c5phy) firmware supports the Seeed XIAO ESP32-C5 only"
#endif

// ============================================================
// Pins — XIAO D-labels
//
// GPIO numbers come from the Arduino core's XIAO_ESP32C5 variant
// (pins_arduino.h): D0=1 D1=0 D2=25 D3=7 D4=23 D5=24 D6=11 D7=12 D8=8 D9=9
// D10=10. The mesh UART stays on D4/D5 exactly as every other station tier.
//
//   label  role                         GPIO
//   D0     I/Q loopback lane   (Q9)     1     LEAVE UNCONNECTED
//   D1     I/Q loopback lane   (Q8)     0     LEAVE UNCONNECTED
//   D2     SP4T control V1              25
//   D3     I/Q loopback lane   (Q6)     7     LEAVE UNCONNECTED
//   D4     UART TX -> Heltec RX         23
//   D5     UART RX <- Heltec TX         24
//   D6     free (UART0 console TX)      11
//   D7     free (UART0 console RX)      12
//   D8     SP4T control V2              8
//   D9     SP4T control V3              9
//   D10    I/Q loopback lane   (I9)     10    LEAVE UNCONNECTED
//   (back pads GPIO2/3/4/5: loopback lanes Q7, I7, I6, I8 — leave unconnected)
//
// The loopback lanes carry the PHY's MODEM_DIAG I/Q bits out through the GPIO
// matrix and straight back into the PARLIO peripheral; the pads must not be
// wired to anything. They are GPIO numbers, not D-labels, because the set is
// the one proven on hardware by the C5VRX project.
// ============================================================
#define PIN_MESH_TX    D4
#define PIN_MESH_RX    D5
#define PIN_SW_V1      D2
#define PIN_SW_V2      D8
#define PIN_SW_V3      D9
#define PIN_STATUS_LED LED_BUILTIN   // GPIO27, active LOW

// MODEM_DIAG lanes -> GPIO, in PARLIO data order: Q[9:6] then I[9:6].
#define IQ_LANE_COUNT  8
#define IQ_LANE_GPIOS  { 1, 0, 2, 7, 10, 5, 3, 4 }
#define IQ_LANE_DIAG   { 6, 7, 8, 9, 16, 17, 18, 19 }

// ============================================================
// Receiver: PHY tuning, gain and I/Q capture
// ============================================================
// PARLIO samples every second MODEM_DIAG sample: 40 MS/s of 4-bit I + 4-bit Q.
#define IQ_SAMPLE_RATE_HZ     40000000u
// One capture window. 16384 samples = 409.6 us = 6.4 video lines, enough for
// 4-5 line periods per window. Must be a multiple of 64 (DMA burst / cache).
#define IQ_WINDOW_BYTES       16384u
#define IQ_CAPTURE_TIMEOUT_MS 20

// Analog filter: 1 = BW40 (the proven receive state), 0 = BW20 (+3 dB, but
// C5VRX found degraded I/Q at boot in BW20). Leave at 1 unless benching it.
#define RF_BW40               1

// Forced PHY receive gain index. 62 = most sensitive. Scanning starts every
// sector at RF_GAIN_MAX and steps down by RF_GAIN_STEP_DOWN while the 4-bit
// I/Q clips, so strong carriers are measured unclipped and weak ones at full
// gain. One index ~= 1 dB (C5VRX measurement, 2026-09-18).
#define RF_GAIN_MAX           62
#define RF_GAIN_MIN           2
#define RF_GAIN_STEP_DOWN     12
#define RF_GAIN_DB_PER_STEP   1.0f
#define RF_CLIP_MAX_PERMILLE  30      // > this share of clipped samples -> gain down
// Mean I^2+Q^2 of a window with no signal at RF_GAIN_MAX (C5VRX: NOISE_POWER 2.0).
// level_db = (RF_GAIN_MAX - gain) * RF_GAIN_DB_PER_STEP + 10 log10(p_mean / this)
#define RF_NOISE_POWER        2.0f

// Settle after a retune / a gain write before the first capture
#define RF_TUNE_SETTLE_MS     8
#define RF_GAIN_SETTLE_MS     3

// Regulatory domain for the Wi-Fi driver. It decides which public 5 GHz
// centres the PHY may be parked on, and every FPV channel is reached from the
// nearest such centre. "" keeps the driver default ("01", world-safe), which
// is the state C5VRX proved 5.8 GHz reception on. If the heartbeat reports
// tune_fail > 0, try the deployment country ("US", "HK", ...). Receive-only:
// no transmit implications either way.
#define RF_COUNTRY_CC         ""

// Scan the LowBand L1-L8 (5362-5621 MHz) too. Off by default: those
// frequencies sit far from any public 5 GHz Wi-Fi centre and are unproven.
#define SCAN_LOW_BAND         0

#if SCAN_LOW_BAND
  #define RX_CHANNEL_COUNT    48
#else
  #define RX_CHANNEL_COUNT    40
#endif

// ============================================================
// Detection
// ============================================================
// Capture windows per sector. The level is the MINIMUM over the windows (a
// Wi-Fi burst lifts one window, a continuous carrier lifts all), coherence
// is the median. Channels that missed QUICK_DWELL_MISSES sweeps in a row get
// the quick count, re-checked at the full count every QUICK_DWELL_RECHECK_SWEEPS.
#define WINDOWS_PER_SECTOR        3
#define WINDOWS_PER_SECTOR_QUICK  1
#define QUICK_DWELL_MISSES        3
#define QUICK_DWELL_RECHECK_SWEEPS 4

// A channel is a hit when, on its strongest sector, the level is at least
// DETECT_LEVEL_DB above the noise reference AND at least DETECT_Q_PHASE_MIN %
// of samples are FM-coherent (phase step within +/-45 deg of the previous
// sample). Wi-Fi/OFDM and thermal noise fail the coherence test even when
// strong, so this is the channel selectivity and the Wi-Fi rejection.
#define DETECT_LEVEL_DB       8.0f
#define DETECT_Q_PHASE_MIN    40

// ============================================================
// Calibration: level_db -> dBm
// ============================================================
// level_db = 0 is "noise-floor power at maximum gain". For a ~20 MHz noise
// bandwidth and a ~6 dB noise figure that is about -95 dBm. UNMEASURED
// default: calibrate with a VTX through a step attenuator (README).
//   rssi_dbm = RSSI_CAL_DBM_AT_NOISE + level_db * RSSI_CAL_SLOPE + RSSI_CAL_OFFSET_DB
#define RSSI_CAL_DBM_AT_NOISE   -95.0f
#define RSSI_CAL_SLOPE          1.0f
#define RSSI_CAL_OFFSET_DB      0.0f    // per-station trim (antenna, cable)

// ============================================================
// Sector antennas / RF switch
// ============================================================
#define SECTOR_COUNT      4

// Azimuth of each sector's patch boresight relative to the enclosure's N face
#define SECTOR_AZIMUTHS   { 0, 90, 180, 270 }

// Control-line levels (V1, V2, V3) that route each sector's RF port to the
// common port. SET THIS FROM THE SWITCH DATASHEET — the default is a plain
// 2-bit binary decode with V3 unused, which matches some SP4Ts and not others.
#define SECTOR_SWITCH_TABLE { {0,0,0}, {1,0,0}, {0,1,0}, {1,1,0} }

// Settle after a switch change before the first capture
#define SECTOR_SETTLE_US  200

// ============================================================
// Bearing (amplitude comparison between the strongest sector's neighbours)
// ============================================================
#define BEARING_DEG_PER_DB      3.0f
#define BEARING_MAX_OFFSET_DEG  45
#define BEARING_SIGMA_BASE_DEG  15
#define BEARING_MIN_MARGIN_DB   6.0f   // peak this far above threshold, else sigma grows

// ============================================================
// Video check (software FM demod + sync scoring on the I/Q windows)
// ============================================================
#define VIDEO_ENABLE          1
#define VIDEO_WINDOWS         8       // windows captured on a peak
#define VIDEO_WINDOW_GAP_MS   5       // spacing between them (spreads over ~100 ms)
#define VIDEO_MAX_PER_SWEEP   2       // strongest peaks that get a video check per sweep
// A window shows video when it holds >= 2 consecutive line periods of
// 64.0 us (PAL) or 63.6 us (NTSC) and its sync score reaches VIDEO_MIN_QUALITY.
// The carrier is reported as video when >= VIDEO_MIN_WINDOWS windows agree.
#define VIDEO_MIN_QUALITY     70
#define VIDEO_MIN_WINDOWS     3

// ============================================================
// Reporting
// ============================================================
#define ENABLE_MESH_RELAY          1
#define REPORT_INTERVAL_MS         5000UL    // per channel, USB
#define MESH_REPORT_INTERVAL_MS    10000UL   // per channel, mesh (LoRa airtime)
#define HEARTBEAT_INTERVAL_MS      60000UL
#define MESH_HEARTBEAT_INTERVAL_MS 120000UL

// Report only the strongest channel of each cluster of adjacent hits
#define PEAK_PICK         1
#define PEAK_WINDOW_MHZ   20

// Mesh line hard limit (Meshtastic text payload)
#define MESH_LINE_MAX     200

// USB bench console ('?' for help). 0 removes it.
#define BENCH_CONSOLE     1
