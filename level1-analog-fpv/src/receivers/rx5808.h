#pragma once
#include "../analog_receiver.h"

// ============================================================
// RX5808 5.8 GHz analog FM receiver (RTC6715 synthesizer) — driver
//
// Wiring (XIAO D-labels, see config.h for GPIO numbers):
//   D10 → RX5808 DATA     D8 → RX5808 CLK     D9 → RX5808 CS (active LOW)
//   D0  ← RX5808 RSSI     3V3 → VCC           GND → GND
//   RX5808 VIDEO → sync separator (see video_sync.h)
//   RX5808 antenna pad ← SP4T common port (see sector_switch.h)
// ============================================================

#define RX_NAME            "rx5808"
#define RX_BAND_LABEL      "5.8G"
#define RX_CHANNEL_COUNT   40
#define RX_TUNE_SETTLE_MS  30      // PLL settle after tuning — do not go below 25

extern const AnalogChannel RX_CHANNELS[];

void  rx_init();
void  rx_tune(uint16_t freq_mhz);
void  rx_read_rssi_stats(RssiStats* out);
float rx_mv_to_dbm(int mv);
float rx_raw_to_dbm(int raw);
