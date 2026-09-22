#pragma once
#include "config.h"

// ============================================================
// demod — what one raw I/Q window says
//
// Two independent looks at a window of IQ_WINDOW_BYTES samples:
//
//  iq_metrics()   power and FM coherence: is there a constant-envelope FM
//                 carrier here, how strong, how far off the tuned frequency,
//                 does it swing like video. This is the detector.
//
//  video_window() software FM discriminator (phase step between samples)
//                 followed by a horizontal-sync search: sync-tip pulses of
//                 the right width repeating at the PAL (64.0 us) or NTSC
//                 (63.6 us) line period. This is the video check and
//                 replaces the sync separator of the v2 station.
//
// Metric definitions follow C5VRX main/video.c (analyze_control_window) and
// the demodulator follows its main/grab.c; both GPL-3.0-only.
// ============================================================

struct IqMetrics {
    float    p_mean;          // mean I^2+Q^2 (4-bit units; noise ~2 at max gain)
    int      p_median;        // median I^2+Q^2, 0..128
    int      q_phase;         // % of samples FM-coherent (|phase step| <= 45 deg, power >= 8)
    int      clip_permille;   // samples with I or Q at full scale
    int      origin_permille; // samples with power <= 4
    int      cfo_khz;         // mean phase step -> carrier offset from the tuned frequency
    bool     cfo_valid;
    bool     noise_like;      // envelope variance says Gaussian noise (only judged when strong)
    bool     modulated;       // phase step swings across the window (video), not a bare CW carrier
};

struct VideoWindow {
    bool  video;              // >= 2 consecutive line periods and quality >= VIDEO_MIN_QUALITY
    bool  pal;                // line period > 63.8 us
    float period_samples;     // mean line period, samples at IQ_SAMPLE_RATE_HZ
    int   quality;            // 0..100 sync score (width 0..40 + period 0..60)
    int   n_periods;          // line periods measured in this window
    int   n_pulses;           // sync-shaped pulses found
    int   swing;              // demodulated swing (sync tip .. 98th percentile), phase units per block
};

void demod_init();                                                  // build the tables once
void iq_metrics(const uint8_t* buf, uint32_t n, IqMetrics* out);
void video_window(const uint8_t* buf, uint32_t n, VideoWindow* out);
