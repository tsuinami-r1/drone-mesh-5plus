#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================
// AnalogReceiver — the seam every receiver driver implements
//
// main.cpp refers only to the RX_* names below. Exactly one receiver is
// selected at build time by a -DRECEIVER_* flag in platformio.ini.
//
// Every receivers/*.h provides:
//   #define RX_NAME            "rx5808"   (JSON "receiver" key)
//   #define RX_BAND_LABEL      "5.8G"     (basic_id prefix)
//   #define RX_CHANNEL_COUNT   40         (#define: sizes stack arrays)
//   #define RX_TUNE_SETTLE_MS  30
//   extern const AnalogChannel RX_CHANNELS[];
//   void  rx_init();
//   void  rx_tune(uint16_t freq_mhz);
//   void  rx_read_rssi_stats(RssiStats* out);   // RSSI_SAMPLES reads, raw + mV
//   float rx_mv_to_dbm(int mv);
//   float rx_raw_to_dbm(int raw);
// ============================================================

struct AnalogChannel {
    uint16_t    freq_mhz;
    const char* band;     // "R","A","B","E","F" (5.8G)  |  "A","B" (3.3G)
    uint8_t     ch;       // 1–8
};

// RSSI sample statistics (one or more dwell reads merged)
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

#if defined(RECEIVER_RX5808)
  #include "receivers/rx5808.h"
#elif defined(RECEIVER_RX3364)
  #error "RX3364 driver not implemented yet; see docs/RX3364-INTEGRATION-PLAN.md"
#else
  #error "Define RECEIVER_RX5808 in platformio.ini build_flags"
#endif
