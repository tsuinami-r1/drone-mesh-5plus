#include "sector_switch.h"

const int16_t SECTOR_AZIMUTH_DEG[SECTOR_COUNT] = SECTOR_AZIMUTHS;

static const uint8_t k_table[SECTOR_COUNT][3] = SECTOR_SWITCH_TABLE;
static uint8_t       g_current = 0xFF;

static_assert(SECTOR_COUNT == 4, "The bearing estimator assumes four 90° sectors");

void sector_init() {
    pinMode(PIN_SW_V1, OUTPUT);
    pinMode(PIN_SW_V2, OUTPUT);
    pinMode(PIN_SW_V3, OUTPUT);
    sector_select(0);
}

void sector_select(uint8_t sector) {
    if (sector >= SECTOR_COUNT) sector = 0;
    if (sector == g_current) return;
    digitalWrite(PIN_SW_V1, k_table[sector][0] ? HIGH : LOW);
    digitalWrite(PIN_SW_V2, k_table[sector][1] ? HIGH : LOW);
    digitalWrite(PIN_SW_V3, k_table[sector][2] ? HIGH : LOW);
    g_current = sector;
    delayMicroseconds(SECTOR_SETTLE_US);
}

uint8_t sector_current() {
    return g_current == 0xFF ? 0 : g_current;
}
