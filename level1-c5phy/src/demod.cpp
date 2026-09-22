// FM coherence metrics and a software sync detector on raw 4-bit I/Q.
//
// Derived from C5VRX main/video.c and main/grab.c
// (github.com/colonelpanichacks/c5vrx), GPL-3.0-only. See LICENSE.

#include "demod.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------
// Tables
// ------------------------------------------------------------
static const int8_t k_sign4[16] = { 0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1 };
static uint8_t s_ang[256];     // angle of a sample byte, 256 units per turn
static uint8_t s_pow[256];     // I^2 + Q^2
static uint8_t s_clp[256];     // 1 when I or Q sits at full scale

static inline int8_t sample_q(uint8_t b) { return k_sign4[b & 0x0f]; }
static inline int8_t sample_i(uint8_t b) { return k_sign4[b >> 4]; }

void demod_init() {
    for (unsigned b = 0; b < 256; b++) {
        int q = sample_q((uint8_t)b);
        int i = sample_i((uint8_t)b);
        s_pow[b] = (uint8_t)(i * i + q * q);
        s_clp[b] = (uint8_t)((i == 7 || i == -8 || q == 7 || q == -8) ? 1 : 0);
        if (i == 0 && q == 0) {
            s_ang[b] = 0;
        } else {
            float a = atan2f((float)q, (float)i) * (128.0f / (float)M_PI);
            s_ang[b] = (uint8_t)((int)lroundf(a) & 0xff);
        }
    }
}

// ------------------------------------------------------------
// Power / coherence metrics
// ------------------------------------------------------------
void iq_metrics(const uint8_t* buf, uint32_t n, IqMetrics* m) {
    memset(m, 0, sizeof(*m));
    if (n < 64) return;

    uint16_t hist[129];
    memset(hist, 0, sizeof(hist));
    uint32_t sum_p = 0;
    uint64_t sum_p2 = 0;
    uint32_t n_clip = 0, n_origin = 0, n_coh = 0;
    int32_t  sum_cross = 0, sum_dot = 0;
    int64_t  sum_cross2 = 0;
    int8_t   pi = sample_i(buf[0]), pq = sample_q(buf[0]);

    {
        uint8_t b = buf[0];
        int p = s_pow[b];
        sum_p += p; sum_p2 += (uint64_t)p * p; n_clip += s_clp[b];
        if (p <= 4) n_origin++;
        hist[p > 128 ? 128 : p]++;
    }
    for (uint32_t k = 1; k < n; k++) {
        uint8_t b = buf[k];
        int8_t i = sample_i(b), q = sample_q(b);
        int p = s_pow[b];
        sum_p += p;
        sum_p2 += (uint64_t)p * p;
        n_clip += s_clp[b];
        if (p <= 4) n_origin++;
        hist[p > 128 ? 128 : p]++;

        // Coherent FM sample: carrier power >= 8 and the phase step from the
        // previous sample within +/-45 deg (|cross| <= dot, dot > 0).
        int dot   = (int)i * pi + (int)q * pq;
        int cross = (int)q * pi - (int)i * pq;
        int ac    = cross < 0 ? -cross : cross;
        if (p >= 8 && dot > 0 && ac <= dot) {
            n_coh++;
            sum_cross  += cross;
            sum_dot    += dot;
            sum_cross2 += (int64_t)cross * cross;
        }
        pi = i; pq = q;
    }

    uint32_t cum = 0;
    for (int p = 0; p <= 128; p++) {
        cum += hist[p];
        if (cum >= (n + 1) / 2) { m->p_median = p; break; }
    }
    m->p_mean          = (float)sum_p / (float)n;
    m->q_phase         = (int)((n_coh * 100u) / (n - 1));
    m->clip_permille   = (int)((n_clip * 1000u) / n);
    m->origin_permille = (int)((n_origin * 1000u) / n);

    // Carrier offset: mean phase step * fs / 2pi. cross/dot ~ tan(step).
    // 40 MS/s / 2pi = 6366 kHz per radian.
    if (n_coh >= 30 && sum_dot > 0) {
        int64_t cfo = ((int64_t)sum_cross * 6366LL) / sum_dot;
        if (cfo >  6000) cfo =  6000;
        if (cfo < -6000) cfo = -6000;
        m->cfo_khz   = (int)cfo;
        m->cfo_valid = true;
    }

    // Envelope: FM is constant-envelope (var/mean^2 << 1), Gaussian noise
    // sits near 1. Only judged when strong enough to tell.
    float var = (float)((double)sum_p2 / (double)n) - m->p_mean * m->p_mean;
    if (var < 0) var = 0;
    m->noise_like = (m->p_mean >= 6.0f) && (var / (m->p_mean * m->p_mean) >= 0.7f);

    // Modulation: video swings the phase step across the window; a CW
    // carrier holds it constant.
    if (n_coh >= 30) {
        float cm = (float)sum_cross / (float)n_coh;
        float cv = (float)((double)sum_cross2 / (double)n_coh) - cm * cm;
        m->modulated = cv > 25.0f;
    }
}

// ------------------------------------------------------------
// Software FM discriminator + horizontal sync search
//
// d[n] = wrapped(ang[n] - ang[n-1]) is the instantaneous frequency in
// 256-units-per-turn per sample (6.4 units = 1 MHz). Sync tip is the lowest
// deviation, white the highest. Block sums over BLK samples give a cheap
// low-pass; the sync/blanking levels come from percentiles of the window.
// ------------------------------------------------------------
#define BLK           16u                                  // 0.4 us
#define MAX_BLOCKS    (IQ_WINDOW_BYTES / BLK)
#define MAX_PULSES    48
#define SPS           (IQ_SAMPLE_RATE_HZ / 1000000u)       // samples per microsecond (40)
#define HSYNC_MIN_BLK 8                                    // 3.2 us
#define HSYNC_MAX_BLK 18                                   // 7.2 us
#define HSYNC_NOMINAL (47 * SPS / 10)                      // 4.7 us = 188 samples
#define PERIOD_MIN    (62 * SPS)                           // 2480
#define PERIOD_MAX    (655 * SPS / 10)                     // 2620
#define PERIOD_PAL    2560.0f                              // 64.0 us
#define PERIOD_NTSC   2542.2f                              // 63.556 us
#define PAL_SPLIT     2551.0f

static int8_t   s_d[IQ_WINDOW_BYTES];
static int16_t  s_blk[MAX_BLOCKS];
static int16_t  s_sorted[MAX_BLOCKS];

struct Pulse { uint32_t start_blk; uint32_t len_blk; float edge; float width; bool refined; };

static int cmp_i16(const void* a, const void* b) {
    return (int)*(const int16_t*)a - (int)*(const int16_t*)b;
}

// First falling crossing of the BLK-sample running sum of d[] below thr in
// [from, to), sub-sample by linear interpolation. -1 if none.
static float falling_edge(uint32_t from, uint32_t to, int thr, uint32_t n) {
    if (from < BLK) from = BLK;
    if (to > n - 1) to = n - 1;
    if (from >= to) return -1.0f;
    int sum = 0;
    for (uint32_t k = from - BLK; k < from; k++) sum += s_d[k];
    int prev = sum;
    for (uint32_t j = from; j < to; j++) {
        sum += s_d[j] - s_d[j - BLK];
        if (prev >= thr && sum < thr) {
            float fr = (float)(prev - thr) / (float)(prev - sum);
            return (float)j + fr - (float)(BLK - 1) * 0.5f;
        }
        prev = sum;
    }
    return -1.0f;
}

// First rising crossing back above thr after 'from'. -1 if none within 'span'.
static float rising_edge(uint32_t from, uint32_t span, int thr, uint32_t n) {
    uint32_t to = from + span;
    if (from < BLK) from = BLK;
    if (to > n - 1) to = n - 1;
    if (from >= to) return -1.0f;
    int sum = 0;
    for (uint32_t k = from - BLK; k < from; k++) sum += s_d[k];
    int prev = sum;
    for (uint32_t j = from; j < to; j++) {
        sum += s_d[j] - s_d[j - BLK];
        if (prev < thr && sum >= thr) {
            float fr = (float)(thr - prev) / (float)(sum - prev);
            return (float)j + fr - (float)(BLK - 1) * 0.5f;
        }
        prev = sum;
    }
    return -1.0f;
}

void video_window(const uint8_t* buf, uint32_t n, VideoWindow* out) {
    memset(out, 0, sizeof(*out));
    if (n > IQ_WINDOW_BYTES) n = IQ_WINDOW_BYTES;
    if (n < 4 * BLK) return;

    // 1. Discriminator
    uint8_t prev = s_ang[buf[0]];
    s_d[0] = 0;
    for (uint32_t k = 1; k < n; k++) {
        uint8_t a = s_ang[buf[k]];
        s_d[k] = (int8_t)(uint8_t)(a - prev);
        prev = a;
    }

    // 2. Block sums (0.4 us low-pass)
    uint32_t nblk = n / BLK;
    for (uint32_t b = 0; b < nblk; b++) {
        int acc = 0;
        const int8_t* p = &s_d[b * BLK];
        for (uint32_t k = 0; k < BLK; k++) acc += p[k];
        s_blk[b] = (int16_t)acc;
    }

    // 3. Levels from percentiles: sync tip = mean of the lowest 5 %, white
    //    end = 98th percentile. A flat window has no video.
    memcpy(s_sorted, s_blk, nblk * sizeof(int16_t));
    qsort(s_sorted, nblk, sizeof(int16_t), cmp_i16);
    uint32_t nlow = nblk / 20;
    if (nlow < 1) nlow = 1;
    int32_t low = 0;
    for (uint32_t k = 0; k < nlow; k++) low += s_sorted[k];
    int sync_lvl = (int)(low / (int32_t)nlow);
    int hi       = s_sorted[nblk * 98 / 100];
    out->swing   = hi - sync_lvl;
    if (hi - sync_lvl < 4 * (int)BLK) return;              // < 4 units/sample of deviation
    int thr = sync_lvl + (hi - sync_lvl) / 5;

    // 4. Pulses: runs of blocks below thr, single-block gaps bridged, runs
    //    touching the window edges dropped.
    Pulse pulses[MAX_PULSES];
    int np = 0;
    for (uint32_t b = 0; b < nblk && np < MAX_PULSES; ) {
        if (s_blk[b] >= thr) { b++; continue; }
        uint32_t s = b;
        while (b < nblk && (s_blk[b] < thr || (b + 1 < nblk && s_blk[b + 1] < thr))) b++;
        if (s > 0 && b < nblk) {
            pulses[np].start_blk = s;
            pulses[np].len_blk   = b - s;
            pulses[np].refined   = false;
            np++;
        }
    }

    // 5. Horizontal-sync candidates: refine the falling edge and measure the width
    int n_sync = 0;
    float width_err_sum = 0;
    for (int k = 0; k < np; k++) {
        Pulse& p = pulses[k];
        if (p.len_blk < HSYNC_MIN_BLK || p.len_blk > HSYNC_MAX_BLK) continue;
        uint32_t s0 = p.start_blk * BLK;
        float e = falling_edge(s0 >= 3 * BLK ? s0 - 3 * BLK : 0, s0 + 2 * BLK, thr, n);
        if (e < 0) continue;
        float r = rising_edge((uint32_t)e + BLK, 12 * BLK + 64, thr, n);
        p.edge    = e;
        p.width   = (r > e) ? (r - e) : (float)(p.len_blk * BLK);
        p.refined = true;
        n_sync++;
        float werr = fabsf(p.width - (float)HSYNC_NOMINAL);
        width_err_sum += werr;
    }
    out->n_pulses = n_sync;
    if (n_sync == 0) return;

    // 6. Line period from consecutive sync pulses (no other pulse between)
    float sum_T = 0;
    int   n_T = 0;
    for (int k = 1; k < np; k++) {
        if (!pulses[k].refined || !pulses[k - 1].refined) continue;
        float T = pulses[k].edge - pulses[k - 1].edge;
        if (T > (float)PERIOD_MIN && T < (float)PERIOD_MAX) { sum_T += T; n_T++; }
    }
    out->n_periods = n_T;

    // 7. Score. Width: 40 at 4.7 us, 0 at +/-2 us. Period: 60 at the nominal
    //    PAL/NTSC value, 0 at +/-15 samples (0.375 us).
    float width_err = width_err_sum / (float)n_sync;
    int width_score = 40 - (int)(width_err * 0.5f);
    if (width_score < 0) width_score = 0;
    int period_score = 0;
    if (n_T >= 1) {
        float T = sum_T / (float)n_T;
        out->period_samples = T;
        out->pal = T > PAL_SPLIT;
        float nominal = out->pal ? PERIOD_PAL : PERIOD_NTSC;
        period_score = 60 - (int)(fabsf(T - nominal) * 4.0f);
        if (period_score < 0) period_score = 0;
    }
    int quality = width_score + period_score;
    if (quality > 100) quality = 100;
    out->quality = quality;
    out->video   = (n_T >= 2) && (quality >= VIDEO_MIN_QUALITY);
}
