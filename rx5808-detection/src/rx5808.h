#pragma once
#include <Arduino.h>

// ============================================================
// RX5808 5.8GHz Analog FM Receiver — Driver
//
// Same physical D-pin connections work on both supported boards.
// GPIO numbers differ between S3 and C5 — selected automatically below.
//
// Physical wiring (use the D-pin label silkscreened on the board):
//   D10  → RX5808 DATA
//   D8   → RX5808 CLK
//   D9   → RX5808 CS   (active LOW)
//   D0   ← RX5808 RSSI (analog, 0–3.3V)
//   3.3V → RX5808 VCC
//   GND  → RX5808 GND
//
//   D4   → Heltec RX   (optional mesh relay)
//   D5   ← Heltec TX   (optional mesh relay)
// ============================================================

// ---- Board-specific GPIO assignments ----
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(ARDUINO_XIAO_ESP32C5)
  // XIAO ESP32-C5: D10=GPIO10, D8=GPIO8, D9=GPIO9, D0=GPIO2
  #define RX5808_DATA_PIN  10  // D10
  #define RX5808_CLK_PIN    8  // D8
  #define RX5808_CS_PIN     9  // D9
  #define RX5808_RSSI_PIN   2  // D0 / ADC1_CH2
#else
  // XIAO ESP32-S3 (default): D10=GPIO9, D8=GPIO7, D9=GPIO8, D0=GPIO1
  #define RX5808_DATA_PIN   9  // D10
  #define RX5808_CLK_PIN    7  // D8
  #define RX5808_CS_PIN     8  // D9
  #define RX5808_RSSI_PIN   1  // D0 / ADC1_CH0
#endif

// ---- Tuning parameters ----

// ADC count (0–4095, 12-bit) above which a signal is reported.
// RX5808 RSSI output swings 0–1 V; with ADC_11db (0–3.1 V range) that is
// ~0–1320 counts. Set threshold ~200 counts above your idle noise floor.
// NOTE: raw counts are board-specific (the S3 and C5 ADCs have different
// transfer curves). The calibrated millivolt/dBm values below are what the
// mapper compares across stations; the raw threshold only gates reporting.
#define RSSI_THRESHOLD    600

// ADC reads averaged per RSSI measurement
#define RSSI_SAMPLES     10

// PLL settle time (ms) after tuning — do not lower below 25 ms
#define TUNE_SETTLE_MS   30

// ---- RSSI calibration: millivolts -> dBm ----
// The RX5808 RSSI pin is close to linear in dBm over ~70 dB. The firmware
// converts the calibrated ADC millivolt reading to dBm with the two-point
// line below and emits it as "rssi_dbm", which is the value the mapper uses
// for range estimation and multi-station fusion (raw counts are not
// comparable between an S3 and a C5).
//
// The defaults are an UNMEASURED approximation of a typical module. To
// calibrate: feed a known level (or a VTX through a step attenuator) on a
// tuned channel, note the "rssi_mv" the station prints at two levels ~50 dB
// apart, and put those (mV, dBm) pairs here. RSSI_CAL_OFFSET_DB is a
// per-station trim for antenna/cable gain so a fielded fleet agrees.
#define RSSI_CAL_MV_LO      450     // mV at RSSI_CAL_DBM_LO (near noise floor)
#define RSSI_CAL_DBM_LO     -95.0f
#define RSSI_CAL_MV_HI      1100    // mV at RSSI_CAL_DBM_HI (strong signal)
#define RSSI_CAL_DBM_HI     -20.0f
#define RSSI_CAL_OFFSET_DB    0.0f  // per-station trim, added to every dBm value

// Nominal ADC full scale at ADC_11db, used only before the first calibrated
// read has established the real mV-per-count ratio for this board.
#define RSSI_NOMINAL_MV_PER_COUNT  (3100.0f / 4095.0f)

// ---- Channel table size (compile-time constant) ----
// Must match the actual number of entries in FPV_CHANNELS[].
// A static_assert in rx5808.cpp enforces this at build time.
#define FPV_CHANNEL_COUNT 40

// ============================================================
// FPV Channel Descriptor
// ============================================================
struct FPVChannel {
    uint16_t    freq_mhz;
    const char* band;     // "R", "A", "B", "E", "F"
    uint8_t     ch;       // 1–8
};

extern const FPVChannel FPV_CHANNELS[];

// ============================================================
// RSSI sample statistics (one or more dwell reads merged)
// ============================================================
struct RssiStats {
    long sum_raw;   // sum of raw 12-bit ADC counts
    long sum_mv;    // sum of calibrated millivolt reads
    int  min_raw;
    int  max_raw;
    int  n;         // number of samples

    void reset()          { sum_raw = 0; sum_mv = 0; min_raw = 4095; max_raw = 0; n = 0; }
    int  mean_raw() const { return n ? (int)(sum_raw / n) : 0; }
    int  mean_mv()  const { return n ? (int)(sum_mv  / n) : 0; }
    void merge(const RssiStats& o) {
        sum_raw += o.sum_raw; sum_mv += o.sum_mv; n += o.n;
        if (o.min_raw < min_raw) min_raw = o.min_raw;
        if (o.max_raw > max_raw) max_raw = o.max_raw;
    }
};

// ============================================================
// Public API
// ============================================================
void  rx5808_init();
void  rx5808_set_frequency(uint16_t freq_mhz);
int   rx5808_read_rssi();                    // mean raw counts of RSSI_SAMPLES reads
void  rx5808_read_rssi_stats(RssiStats* out); // RSSI_SAMPLES reads, raw + mV, min/max
float rx5808_mv_to_dbm(int mv);              // calibration line above
float rx5808_raw_to_dbm(int raw);            // via this board's measured mV/count ratio
