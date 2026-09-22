// PARLIO RX one-shot capture of the MODEM_DIAG I/Q lanes.
//
// PARLIO configuration derived from C5VRX main/video.c (prepare_rx), which is
// the receive setup proven on hardware: 8-bit width, internal PLL_F240M clock
// divided to 40 MHz, positive sample edge, LSB packing, soft delimiter.
// GPL-3.0-only, see LICENSE in this directory.

#include "iq_capture.h"

#include "driver/parlio_rx.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"

static parlio_rx_unit_handle_t      s_rx    = nullptr;
static parlio_rx_delimiter_handle_t s_delim = nullptr;
static uint8_t*                     s_buf   = nullptr;
static uint32_t                     s_captures = 0;
static uint32_t                     s_errors   = 0;

static const int k_iq_gpio[IQ_LANE_COUNT] = IQ_LANE_GPIOS;

static_assert(IQ_LANE_COUNT == 8, "PARLIO data width is 8: Q[9:6] + I[9:6]");
static_assert((IQ_WINDOW_BYTES % 64u) == 0u, "IQ_WINDOW_BYTES must be a multiple of 64");

bool iq_init() {
    if (s_rx) return true;

    // DMA-capable, 64-byte aligned internal SRAM (the upper 128 KB of the
    // C5's SRAM is not DMA-addressable; heap_caps picks a valid region).
    s_buf = (uint8_t*)heap_caps_aligned_alloc(64, IQ_WINDOW_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf) return false;

    parlio_rx_unit_config_t cfg = {};
    cfg.trans_queue_depth = 1;
    cfg.max_recv_size     = IQ_WINDOW_BYTES;
    cfg.dma_burst_size    = 32;
    cfg.data_width        = 8;
    cfg.clk_src           = PARLIO_CLK_SRC_DEFAULT;
    cfg.ext_clk_freq_hz   = 0;
    cfg.exp_clk_freq_hz   = IQ_SAMPLE_RATE_HZ;
    cfg.clk_in_gpio_num   = GPIO_NUM_NC;
    cfg.clk_out_gpio_num  = GPIO_NUM_NC;
    cfg.valid_gpio_num    = GPIO_NUM_NC;
    for (int i = 0; i < PARLIO_RX_UNIT_MAX_DATA_WIDTH; i++)
        cfg.data_gpio_nums[i] = (i < IQ_LANE_COUNT) ? (gpio_num_t)k_iq_gpio[i] : GPIO_NUM_NC;
    cfg.flags.free_clk    = 1;
    cfg.flags.clk_gate_en = 0;
    cfg.flags.allow_pd    = 0;
    if (parlio_new_rx_unit(&cfg, &s_rx) != ESP_OK) { s_rx = nullptr; return false; }

    parlio_rx_soft_delimiter_config_t dcfg = {};
    dcfg.sample_edge    = PARLIO_SAMPLE_EDGE_POS;
    dcfg.bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB;
    dcfg.eof_data_len   = IQ_WINDOW_BYTES;
    dcfg.timeout_ticks  = 0;
    if (parlio_new_rx_soft_delimiter(&dcfg, &s_delim) != ESP_OK) return false;

    return parlio_rx_unit_enable(s_rx, false) == ESP_OK;
}

bool iq_capture(const uint8_t** out) {
    *out = nullptr;
    if (!s_rx || !s_delim) return false;

    parlio_receive_config_t rcfg = {};
    rcfg.delimiter            = s_delim;
    rcfg.flags.partial_rx_en  = 0;
    rcfg.flags.indirect_mount = 0;

    // Driver sequence for a soft delimiter: queue the transaction (mounts the
    // DMA descriptors), then start the soft receive so data flows into a
    // ready DMA rather than an unattended FIFO, wait for the EOF, stop.
    esp_err_t err = parlio_rx_unit_receive(s_rx, s_buf, IQ_WINDOW_BYTES, &rcfg);
    if (err == ESP_OK) err = parlio_rx_soft_delimiter_start_stop(s_rx, s_delim, true);
    if (err == ESP_OK) err = parlio_rx_unit_wait_all_done(s_rx, IQ_CAPTURE_TIMEOUT_MS);
    (void)parlio_rx_soft_delimiter_start_stop(s_rx, s_delim, false);
    if (err != ESP_OK) {
        s_errors++;
        // A stuck transaction is the usual failure; reset the queue so the
        // next capture starts clean.
        (void)parlio_rx_unit_disable(s_rx);
        (void)parlio_rx_unit_enable(s_rx, true);
        return false;
    }

    // The DMA wrote through the bus; drop any stale cache lines before the
    // CPU reads (no-op where the SoC keeps this SRAM coherent).
    (void)esp_cache_msync(s_buf, IQ_WINDOW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    s_captures++;
    *out = s_buf;
    return true;
}

uint32_t iq_capture_count() { return s_captures; }
uint32_t iq_error_count()   { return s_errors; }
