#pragma once
#include "config.h"

// ============================================================
// iq_capture — one-shot raw I/Q windows from the PHY via PARLIO RX
//
// The eight MODEM_DIAG lanes routed by c5phy_rf are read back by the PARLIO
// peripheral at IQ_SAMPLE_RATE_HZ (every second modem sample) into a DMA
// buffer. Each byte is one sample: I in the high nibble, Q in the low nibble,
// both 4-bit two's complement.
//
// Captures are finite and blocking (about 0.4 ms for IQ_WINDOW_BYTES), which
// keeps the DMA idle while the PHY is retuned and the gain is changed.
// ============================================================

// Create the PARLIO unit and the DMA buffer. False on failure.
bool iq_init();

// Capture one window into the internal buffer and return it. The pointer is
// valid until the next iq_capture(). False on timeout / driver error.
bool iq_capture(const uint8_t** out);

// Diagnostics
uint32_t iq_capture_count();
uint32_t iq_error_count();
