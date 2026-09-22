#pragma once
#include "config.h"

// ============================================================
// Amplitude-comparison direction finding across the four sectors
// ============================================================

struct Bearing {
    int16_t deg;        // 0–359, relative to true north (includes STATION_HEADING_DEG)
    uint8_t sigma_deg;  // 1σ estimate of the bearing error
    uint8_t sector;     // strongest sector index
};

// sector_dbm[] are the calibrated powers seen on each sector for one channel.
// threshold_dbm is the station's detection threshold in dBm, used to judge
// whether the neighbours carry real signal.
Bearing bearing_estimate(const float sector_dbm[SECTOR_COUNT], float threshold_dbm);
