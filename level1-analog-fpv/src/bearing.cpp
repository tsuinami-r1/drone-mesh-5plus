#include "bearing.h"
#include "sector_switch.h"

Bearing bearing_estimate(const float sector_dbm[SECTOR_COUNT], float threshold_dbm) {
    // Strongest sector gives the quadrant
    uint8_t k = 0;
    for (uint8_t s = 1; s < SECTOR_COUNT; s++)
        if (sector_dbm[s] > sector_dbm[k]) k = s;

    const uint8_t left  = (k + SECTOR_COUNT - 1) % SECTOR_COUNT;
    const uint8_t right = (k + 1) % SECTOR_COUNT;
    const uint8_t back  = (k + 2) % SECTOR_COUNT;

    // Offset inside the quadrant from the neighbour imbalance, in dB.
    // Positive = towards the right-hand (clockwise) neighbour.
    float delta  = sector_dbm[right] - sector_dbm[left];
    float offset = delta * BEARING_DEG_PER_DB;
    if (offset >  BEARING_MAX_OFFSET_DEG) offset =  BEARING_MAX_OFFSET_DEG;
    if (offset < -BEARING_MAX_OFFSET_DEG) offset = -BEARING_MAX_OFFSET_DEG;

    int deg = SECTOR_AZIMUTH_DEG[k] + STATION_HEADING_DEG + (int)lroundf(offset);
    deg = ((deg % 360) + 360) % 360;

    // Confidence: start at the pattern's own uncertainty, widen when the
    // measurement cannot support the estimate.
    int sigma = BEARING_SIGMA_BASE_DEG;
    const float margin = sector_dbm[k] - threshold_dbm;
    if (margin < BEARING_MIN_MARGIN_DB) sigma += 15;                 // weak peak
    if (sector_dbm[left] < threshold_dbm && sector_dbm[right] < threshold_dbm)
        sigma += 10;                                                 // neighbours are noise floor
    if (sector_dbm[k] - sector_dbm[back] < 3.0f) sigma += 20;        // rear sector nearly equal: multipath
    if (sigma > 90) sigma = 90;

    Bearing b;
    b.deg       = (int16_t)deg;
    b.sigma_deg = (uint8_t)sigma;
    b.sector    = k;
    return b;
}
