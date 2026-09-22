#pragma once
#include "config.h"

// ============================================================
// c5phy_rf — the ESP32-C5 Wi-Fi PHY as a receive-only 5.8 GHz front end
//
// Brings the Wi-Fi driver up in station mode with no power saving, disables
// every MAC transmit queue, parks the PHY on the public 5 GHz centre nearest
// the wanted FPV channel and then retunes it to the exact frequency, routes
// the modem's raw I/Q diagnostic bus out through eight GPIO-matrix lanes
// (which iq_capture reads back with PARLIO), and holds the receive gain at a
// fixed, known index so power can be turned into dBm.
//
// Derived from C5VRX (github.com/colonelpanichacks/c5vrx, GPL-3.0-only):
// the register sequence, the MODEM_DIAG lane map and the undocumented PHY
// entry points are that project's hardware findings on ESP-IDF 6.0; this file
// uses the same symbols from the pioarduino 55.03.39 (ESP-IDF 5.5) libphy.
// ============================================================

struct RfStatus {
    bool     started;
    uint16_t tuned_mhz;      // exact frequency requested from the PHY
    uint8_t  wifi_channel;   // public 5 GHz centre used as the bootstrap
    uint8_t  gain;           // forced receive gain index
    bool     bw40;
};

// Bring the front end up. False on a hard failure (reason in rf_last_error()).
bool rf_start();

// Tune to an FPV frequency. Returns false when no public 5 GHz centre near it
// is accepted by the regulatory table (the channel is then unusable here).
bool rf_tune(uint16_t freq_mhz);

// Fixed receive gain index, RF_GAIN_MIN..RF_GAIN_MAX. No-op if unchanged.
void    rf_set_gain(uint8_t idx);
uint8_t rf_gain();

// Analog filter: true = BW40, false = BW20
void rf_set_bw40(bool bw40);

// PHY-reported wideband RSSI / noise floor in dBm, false if implausible
bool rf_phy_rssi_dbm(int* dbm);
bool rf_phy_noise_dbm(int* dbm);

// Re-assert the MODEM_DIAG -> GPIO lane routing (call after iq_init(), which
// reconfigures those GPIOs as PARLIO inputs).
void rf_route_iq_lanes();

const RfStatus& rf_status();
const char*     rf_last_error();
