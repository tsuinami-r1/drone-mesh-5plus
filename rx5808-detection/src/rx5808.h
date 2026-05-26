#pragma once
#include <Arduino.h>

// ============================================================
// RX5808 5.8GHz Analog FM Receiver — Driver
//
// XIAO ESP32-S3 Wiring:
//   GPIO9  (D10/MOSI) → RX5808 DATA
//   GPIO7  (D8/SCK)   → RX5808 CLK
//   GPIO8  (D9)       → RX5808 CS  (active LOW)
//   GPIO1  (D0/ADC)   ← RX5808 RSSI (analog output)
//   3.3V              → RX5808 VCC
//   GND               → RX5808 GND
//
// GPIO5 / GPIO6 are reserved for the optional Heltec UART relay.
// ============================================================

#define RX5808_DATA_PIN  9   // SPI MOSI (bit-bang)
#define RX5808_CLK_PIN   7   // SPI clock
#define RX5808_CS_PIN    8   // Chip select, active LOW
#define RX5808_RSSI_PIN  1   // ADC1_CH0 — analog RSSI input

// ADC signal threshold (0–4095, 12-bit).
// The RX5808 RSSI output rises with signal strength.
// Calibrate by checking idle noise floor and raising until false positives stop.
#define RSSI_THRESHOLD   1800

// Number of ADC samples averaged per RSSI reading
#define RSSI_SAMPLES     10

// PLL settle time (ms) after tuning before reading RSSI
#define TUNE_SETTLE_MS   30

// ============================================================
// FPV Channel Descriptor
// ============================================================
struct FPVChannel {
    uint16_t    freq_mhz;
    const char* band;     // "R", "A", "B", "E", "F"
    uint8_t     ch;       // 1–8
};

extern const FPVChannel FPV_CHANNELS[];
extern const int        FPV_CHANNEL_COUNT;

// ============================================================
// Public API
// ============================================================
void rx5808_init();
void rx5808_set_frequency(uint16_t freq_mhz);
int  rx5808_read_rssi();
