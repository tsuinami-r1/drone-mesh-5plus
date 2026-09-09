#pragma once
#include "config.h"

// ============================================================
// Video sync measurement — is this carrier really analog video, and which
// standard? Counts the sync separator's composite-sync (CSYNC, D6) and
// vertical-sync (VSYNC, D7) edges for VIDEO_MEASURE_MS while the receiver
// stays tuned to the carrier.
//
// The standard is decided by the FIELD rate (50 Hz PAL / 60 Hz NTSC). The
// line rate only has to fall inside a video-shaped window, because an
// LM1881's CSYNC includes equalising and serration pulses during vertical
// blanking that push the edge rate ~500 Hz above the nominal line rate.
// ============================================================

struct VideoResult {
    const char* standard;   // "NTSC", "PAL", or "none"
    uint16_t    line_hz;    // measured CSYNC edge rate
    uint8_t     field_hz;   // measured VSYNC edge rate
    uint8_t     quality;    // 0–100: share of 10 ms slices whose count matched the median
    bool        present;    // standard != "none"
};

void        video_init();
VideoResult video_measure();     // blocks for VIDEO_MEASURE_MS
