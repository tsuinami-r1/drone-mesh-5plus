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
  #define RX5808_RSSI_PIN   2  // D0 / ADC1_CH1
#else
  // XIAO ESP32-S3 (default): D10=GPIO9, D8=GPIO7, D9=GPIO8, D0=GPIO1
  #define RX5808_DATA_PIN   9  // D10
  #define RX5808_CLK_PIN    7  // D8
  #define RX5808_CS_PIN     8  // D9
  #define RX5808_RSSI_PIN   1  // D0 / ADC1_CH0
#endif

// ---- Tuning parameters ----

// ADC count (0–4095, 12-bit) above which a signal is reported.
// Set this ~200 counts above your observed idle noise floor.
#define RSSI_THRESHOLD   1800

// ADC reads averaged per RSSI measurement
#define RSSI_SAMPLES     10

// PLL settle time (ms) after tuning — do not lower below 25 ms
#define TUNE_SETTLE_MS   30

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
// Public API
// ============================================================
void rx5808_init();
void rx5808_set_frequency(uint16_t freq_mhz);
int  rx5808_read_rssi();
