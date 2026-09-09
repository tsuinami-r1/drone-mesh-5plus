#include "video_sync.h"

static volatile uint32_t g_csync = 0;
static volatile uint32_t g_vsync = 0;

static void IRAM_ATTR isr_csync() { g_csync = g_csync + 1; }
static void IRAM_ATTR isr_vsync() { g_vsync = g_vsync + 1; }

void video_init() {
    // Pull-downs keep an unpopulated sync separator from generating noise
    // edges; a real separator drives the lines push-pull.
    pinMode(PIN_CSYNC, INPUT_PULLDOWN);
    pinMode(PIN_VSYNC, INPUT_PULLDOWN);
}

static uint16_t median_of(uint16_t* v, int n) {
    // n is small (VIDEO_MEASURE_MS / VIDEO_SLICE_MS); insertion sort a copy
    uint16_t tmp[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) tmp[i] = v[i];
    for (int i = 1; i < n; i++) {
        uint16_t x = tmp[i]; int j = i - 1;
        while (j >= 0 && tmp[j] > x) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = x;
    }
    return tmp[n / 2];
}

VideoResult video_measure() {
    VideoResult r;
    r.standard = "none";
    r.line_hz  = 0;
    r.field_hz = 0;
    r.quality  = 0;
    r.present  = false;

#if VIDEO_ENABLE
    constexpr int slices = VIDEO_MEASURE_MS / VIDEO_SLICE_MS;
    static_assert(slices >= 5 && slices <= 64, "VIDEO_MEASURE_MS / VIDEO_SLICE_MS must be 5..64");
    uint16_t slice_counts[64];

    g_csync = 0;
    g_vsync = 0;
    attachInterrupt(digitalPinToInterrupt(PIN_CSYNC), isr_csync, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_VSYNC), isr_vsync, FALLING);

    uint32_t last = g_csync;
    for (int i = 0; i < slices; i++) {
        delay(VIDEO_SLICE_MS);
        uint32_t now = g_csync;
        uint32_t d   = now - last;
        slice_counts[i] = d > 65535 ? 65535 : (uint16_t)d;
        last = now;
    }
    detachInterrupt(digitalPinToInterrupt(PIN_CSYNC));
    detachInterrupt(digitalPinToInterrupt(PIN_VSYNC));

    uint32_t csync_total = g_csync;
    uint32_t vsync_total = g_vsync;

    r.line_hz  = (uint16_t)((csync_total * 1000UL) / VIDEO_MEASURE_MS);
    r.field_hz = (uint8_t)((vsync_total * 1000UL) / VIDEO_MEASURE_MS);

    // Quality: slices within ±3 counts of the median slice (a stable train
    // gives ~157 per 10 ms; dropouts and noise scatter it).
    uint16_t med = median_of(slice_counts, slices);
    int ok = 0;
    for (int i = 0; i < slices; i++) {
        int diff = (int)slice_counts[i] - (int)med;
        if (diff >= -3 && diff <= 3) ok++;
    }
    r.quality = (uint8_t)((ok * 100) / slices);

    const bool line_ok = (r.line_hz >= VIDEO_LINE_HZ_MIN && r.line_hz <= VIDEO_LINE_HZ_MAX);
    if (line_ok && (int)r.field_hz >= 60 - VIDEO_FIELD_TOL_HZ && (int)r.field_hz <= 60 + VIDEO_FIELD_TOL_HZ) {
        r.standard = "NTSC"; r.present = true;
    } else if (line_ok && (int)r.field_hz >= 50 - VIDEO_FIELD_TOL_HZ && (int)r.field_hz <= 50 + VIDEO_FIELD_TOL_HZ) {
        r.standard = "PAL";  r.present = true;
    }
#endif
    return r;
}
