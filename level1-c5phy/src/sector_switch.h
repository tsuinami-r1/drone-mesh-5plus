#pragma once
#include "config.h"

// ============================================================
// SP4T RF switch — routes one of four sector patch antennas to the receiver
//
// Three control lines (PIN_SW_V1/V2/V3 in config.h: D2/D8/D9 on the v3
// station) drive the switch's V1/V2/V3 per the SECTOR_SWITCH_TABLE. Sector s
// points at SECTOR_AZIMUTH_DEG[s] relative to the enclosure's N face.
// ============================================================

extern const int16_t SECTOR_AZIMUTH_DEG[SECTOR_COUNT];

void    sector_init();
void    sector_select(uint8_t sector);   // drives the lines and waits SECTOR_SETTLE_US
uint8_t sector_current();
