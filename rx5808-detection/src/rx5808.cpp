#include "rx5808.h"

// Standard FPV bands — Raceband, A, B, E, F (40 channels total).
// Frequencies follow the community-standard channel plan.
// Raceband R7 and Band F8 both land on 5880 MHz (same physical frequency).
const FPVChannel FPV_CHANNELS[] = {
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
const int FPV_CHANNEL_COUNT = (int)(sizeof(FPV_CHANNELS) / sizeof(FPV_CHANNELS[0]));

// Shift 'bits' bits of 'data' out LSB-first on the bit-bang SPI bus.
static void spi_shift_out(uint32_t data, int bits) {
    for (int i = 0; i < bits; i++) {
        digitalWrite(RX5808_CLK_PIN, LOW);
        digitalWrite(RX5808_DATA_PIN, (data >> i) & 1);
        delayMicroseconds(2);
        digitalWrite(RX5808_CLK_PIN, HIGH);
        delayMicroseconds(2);
    }
    digitalWrite(RX5808_CLK_PIN, LOW);
}

void rx5808_init() {
    pinMode(RX5808_DATA_PIN, OUTPUT);
    pinMode(RX5808_CLK_PIN,  OUTPUT);
    pinMode(RX5808_CS_PIN,   OUTPUT);

    digitalWrite(RX5808_DATA_PIN, LOW);
    digitalWrite(RX5808_CLK_PIN,  LOW);
    digitalWrite(RX5808_CS_PIN,   HIGH);  // deselected

    // 0–3.3V attenuation covers the full RX5808 RSSI output range
    analogSetAttenuation(ADC_11db);
    analogReadResolution(12);
}

void rx5808_set_frequency(uint16_t freq_mhz) {
    // Synthesizer B register data: standard formula used by RotorHazard / Chorus RF Laptimer.
    // Frequency range: 5645–5945 MHz.
    uint16_t reg_val = (freq_mhz - 479) / 2;

    // 25-bit SPI word layout (LSB first):
    //   bits  [3:0] = register address 0x01 (Synthesizer B)
    //   bit   [4]   = R/W flag, 1 = write
    //   bits [24:5] = 20-bit register data (upper 4 bits of reg_val are always 0
    //                 for the 5.8GHz FPV band, so 16 bits are sufficient)
    uint32_t spi_word = 0x01u | (1u << 4) | ((uint32_t)reg_val << 5);

    digitalWrite(RX5808_CS_PIN, LOW);
    spi_shift_out(spi_word, 25);
    digitalWrite(RX5808_CS_PIN, HIGH);
}

int rx5808_read_rssi() {
    long sum = 0;
    for (int i = 0; i < RSSI_SAMPLES; i++) {
        sum += analogRead(RX5808_RSSI_PIN);
        delayMicroseconds(200);
    }
    return (int)(sum / RSSI_SAMPLES);
}
