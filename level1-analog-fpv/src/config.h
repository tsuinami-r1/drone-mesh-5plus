#pragma once
#include <Arduino.h>

// ============================================================
// Level 1 station v2 — configuration
//
// Every tuneable and every pin lives here. The scan loop (main.cpp), the
// receiver driver (receivers/), the sector switch, the bearing estimator and
// the video-sync measurement read only this file.
// ============================================================

// ---- Station identity ----
// Change per device. Must equal the paired Meshtastic node's shortName /
// longName, or the mapper draws this station's output in the wrong place.
#define NODE_ID          "RX01"

// Hardware revision reported in every heartbeat ("hw")
#define STATION_HW       "v2"

// True bearing (degrees) of the enclosure face carrying patch N. Bearings are
// reported relative to true north, so this must be measured at install time.
#define STATION_HEADING_DEG   0

// ============================================================
// Pins — XIAO D-labels
//
// GPIO numbers come from the Arduino core's XIAO variant (pins_arduino.h), so
// the same source builds for both boards and follows the board's own pin map:
//
//   label  role            XIAO ESP32-S3   XIAO ESP32-C5
//   D0     RSSI (ADC)      GPIO1           GPIO1
//   D1     switch V1       GPIO2           GPIO0
//   D2     switch V2       GPIO3           GPIO25
//   D3     switch V3       GPIO4           GPIO7
//   D4     UART TX→Heltec  GPIO5           GPIO23
//   D5     UART RX←Heltec  GPIO6           GPIO24
//   D6     CSYNC in        GPIO43          GPIO11
//   D7     VSYNC in        GPIO44          GPIO12
//   D8     RX5808 CLK      GPIO7           GPIO8
//   D9     RX5808 CS       GPIO8           GPIO9
//   D10    RX5808 DATA     GPIO9           GPIO10
//
// NOTE (C5): earlier firmware on this branch hard-coded D0=GPIO2, D4=GPIO6,
// D5=GPIO7 for the C5, which disagrees with the variant above. The variant's
// D6/D7 equal the C5's UART0 defaults exactly as the S3's do, so it is the
// one used here. Confirm with a continuity test on the first C5 v2 board.
// ============================================================
#if !defined(ARDUINO_XIAO_ESP32S3) && !defined(ARDUINO_XIAO_ESP32C5)
  #error "Level 1 v2 firmware supports the Seeed XIAO ESP32-S3 and XIAO ESP32-C5 only"
#endif

#define PIN_RSSI       D0
#define PIN_SW_V1      D1
#define PIN_SW_V2      D2
#define PIN_SW_V3      D3
#define PIN_MESH_TX    D4
#define PIN_MESH_RX    D5
#define PIN_CSYNC      D6
#define PIN_VSYNC      D7
#define PIN_RX_CLK     D8
#define PIN_RX_CS      D9
#define PIN_RX_DATA    D10

// ============================================================
// Receiver
// ============================================================
// Raw ADC count (0–4095) above which a channel is considered a hit. Raw counts
// are board-specific and only gate reporting; the calibrated dBm value is what
// the mapper compares. RX5808 RSSI spans ~0–1320 counts.
#define RSSI_THRESHOLD    600

// ADC reads per dwell read (raw count + calibrated millivolts each)
#define RSSI_SAMPLES      10

// Dwell reads on the strongest sector that must all clear the threshold
#define MIN_DWELL_HITS    2

// RSSI calibration: millivolts → dBm, two-point line. UNMEASURED defaults;
// see README "Calibration". RSSI_CAL_OFFSET_DB is the per-station trim.
#define RSSI_CAL_MV_LO       450
#define RSSI_CAL_DBM_LO      -95.0f
#define RSSI_CAL_MV_HI       1100
#define RSSI_CAL_DBM_HI      -20.0f
#define RSSI_CAL_OFFSET_DB   0.0f

// Nominal ADC full scale at ADC_11db, used before the first calibrated read
#define RSSI_NOMINAL_MV_PER_COUNT  (3100.0f / 4095.0f)

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

// Settle after a switch change before reading RSSI (switch + RSSI filter)
#define SECTOR_SETTLE_US  200

// ============================================================
// Bearing (amplitude comparison between the strongest sector's neighbours)
// ============================================================
// Degrees of bearing offset per dB of (right neighbour − left neighbour).
// ~3 °/dB for an 8 dBi / 70° patch; calibrate with a VTX walked around the
// box in 15° steps and keep the table in docs/.
#define BEARING_DEG_PER_DB      3.0f
#define BEARING_MAX_OFFSET_DEG  45
#define BEARING_SIGMA_BASE_DEG  15
#define BEARING_MIN_MARGIN_DB   6.0f   // peak this far above threshold, else sigma grows

// ============================================================
// Video sync (sync separator on the RX5808 video output)
// ============================================================
#define VIDEO_ENABLE          1
#define VIDEO_MEASURE_MS      200     // total window; CSYNC is counted in 10 ms slices
#define VIDEO_SLICE_MS        10
#define VIDEO_MAX_PER_SWEEP   2       // strongest peaks that get fine-tune + video per sweep

// Composite-sync rate window that counts as "a video carrier". Wide enough to
// include an LM1881's equalising/serration pulses (+~500 Hz); the standard is
// decided by the field rate, not the line rate.
#define VIDEO_LINE_HZ_MIN     14500
#define VIDEO_LINE_HZ_MAX     17000
#define VIDEO_FIELD_TOL_HZ    3       // 50±3 → PAL, 60±3 → NTSC

// ============================================================
// Fine tune (locates the carrier centre for the fingerprint)
// ============================================================
#define FINE_TUNE_ENABLE      1
#define FINE_TUNE_SPAN_MHZ    10      // ± around the hit channel
#define FINE_TUNE_STEP_MHZ    2       // RTC6715 synthesizer step

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
