#include "rx5808.h"

// Standard FPV bands — Raceband, A, B, E, F.
// Raceband R7 and Band F8 both land on 5880 MHz (same physical frequency).
const AnalogChannel RX_CHANNELS[] = {
    // Raceband
    {5658, "R", 1}, {5695, "R", 2}, {5732, "R", 3}, {5769, "R", 4},
    {5806, "R", 5}, {5843, "R", 6}, {5880, "R", 7}, {5917, "R", 8},
    // Band A
    {5865, "A", 1}, {5845, "A", 2}, {5825, "A", 3}, {5805, "A", 4},
    {5785, "A", 5}, {5765, "A", 6}, {5745, "A", 7}, {5725, "A", 8},
    // Band B
    {5733, "B", 1}, {5752, "B", 2}, {5771, "B", 3}, {5790, "B", 4},
    {5809, "B", 5}, {5828, "B", 6}, {5847, "B", 7}, {5866, "B", 8},
    // Band E
    {5705, "E", 1}, {5685, "E", 2}, {5665, "E", 3}, {5645, "E", 4},
    {5885, "E", 5}, {5905, "E", 6}, {5925, "E", 7}, {5945, "E", 8},
    // Band F (Fatshark)
    {5740, "F", 1}, {5760, "F", 2}, {5780, "F", 3}, {5800, "F", 4},
    {5820, "F", 5}, {5840, "F", 6}, {5860, "F", 7}, {5880, "F", 8},
};
static_assert(sizeof(RX_CHANNELS) / sizeof(RX_CHANNELS[0]) == RX_CHANNEL_COUNT,
              "RX_CHANNEL_COUNT does not match RX_CHANNELS array length");
static_assert(RSSI_CAL_MV_HI > RSSI_CAL_MV_LO,
              "RSSI calibration points must be in increasing mV order");

// Measured mV-per-count ratio for THIS board's ADC, refined on every stats
// read, so raw thresholds can be expressed in dBm without knowing the chip.
static float g_mv_per_count = RSSI_NOMINAL_MV_PER_COUNT;

// Shift 'bits' bits of 'data' out LSB-first on the bit-bang SPI bus.
static void spi_shift_out(uint32_t data, int bits) {
    for (int i = 0; i < bits; i++) {
        digitalWrite(PIN_RX_CLK, LOW);
        digitalWrite(PIN_RX_DATA, (data >> i) & 1);
        delayMicroseconds(2);
        digitalWrite(PIN_RX_CLK, HIGH);
        delayMicroseconds(2);
    }
    digitalWrite(PIN_RX_CLK, LOW);
}

void rx_init() {
    pinMode(PIN_RX_DATA, OUTPUT);
    pinMode(PIN_RX_CLK,  OUTPUT);
    pinMode(PIN_RX_CS,   OUTPUT);
    digitalWrite(PIN_RX_DATA, LOW);
    digitalWrite(PIN_RX_CLK,  LOW);
    digitalWrite(PIN_RX_CS,   HIGH);  // deselected

    // Per-pin attenuation: covers the full 0–1 V RX5808 RSSI swing with margin.
    analogSetPinAttenuation(PIN_RSSI, ADC_11db);
    analogReadResolution(12);
}

void rx_tune(uint16_t freq_mhz) {
    // Synthesizer Register B (RTC6715) is a SPLIT field, not a flat value:
    //   bits [6:0]  = A counter (7 bits)
    //   bits [19:7] = N counter (13 bits)
    //   tuned RF = 2 * (N * 32 + A) + 479 MHz   (RotorHazard / Chorus formula)
    // Writing the raw (freq-479)/2 without the N/A split mistunes the receiver
    // by ~4 GHz (e.g. 5658 MHz -> ~1817 MHz), so nothing is ever detected.
    uint16_t tf      = (freq_mhz - 479) / 2;
    uint16_t reg_val = ((tf / 32) << 7) | (tf % 32);

    // 25-bit SPI word (LSB first):
    //   bits  [3:0] = register address 0x01 (Synthesizer B)
    //   bit   [4]   = R/W flag (1 = write)
    //   bits [24:5] = 20-bit register data
    uint32_t spi_word = 0x01u | (1u << 4) | ((uint32_t)reg_val << 5);

    digitalWrite(PIN_RX_CS, LOW);
    spi_shift_out(spi_word, 25);
    digitalWrite(PIN_RX_CS, HIGH);
}

void rx_read_rssi_stats(RssiStats* out) {
    out->reset();
    for (int i = 0; i < RSSI_SAMPLES; i++) {
        // Raw counts keep the threshold scale; the eFuse-calibrated millivolt
        // read is what makes S3 and C5 stations comparable and feeds dBm.
        int raw = analogRead(PIN_RSSI);
        int mv  = (int)analogReadMilliVolts(PIN_RSSI);
        out->sum_raw += raw;
        out->sum_mv  += mv;
        if (raw < out->min_raw) out->min_raw = raw;
        if (raw > out->max_raw) out->max_raw = raw;
        out->n++;
        delayMicroseconds(200);
    }
    // Refine the board's mV/count ratio from real signal (ignore near-zero
    // reads where quantisation dominates).
    if (out->sum_raw > 200L * out->n) {
        g_mv_per_count = (float)out->sum_mv / (float)out->sum_raw;
    }
}

float rx_mv_to_dbm(int mv) {
    const float slope = (RSSI_CAL_DBM_HI - RSSI_CAL_DBM_LO)
                      / (float)(RSSI_CAL_MV_HI - RSSI_CAL_MV_LO);
    float dbm = RSSI_CAL_DBM_LO + (mv - RSSI_CAL_MV_LO) * slope + RSSI_CAL_OFFSET_DB;
    if (dbm < -120.0f) dbm = -120.0f;
    if (dbm >    0.0f) dbm =    0.0f;
    return dbm;
}

float rx_raw_to_dbm(int raw) {
    return rx_mv_to_dbm((int)(raw * g_mv_per_count));
}
