// ESP32-C5 Wi-Fi PHY as a receive-only analog FM front end.
//
// Derived from C5VRX main/rf.c (github.com/colonelpanichacks/c5vrx),
// GPL-3.0-only. See LICENSE in this directory.

#include "c5phy_rf.h"

#include <string.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_rom_gpio.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"

// ------------------------------------------------------------
// Undocumented PHY entry points.
//
// Strong references on purpose: the pinned framework's libphy.a exports every
// one of these (checked with nm on pioarduino 55.03.39 / ESP-IDF 5.5), and a
// platform bump that drops one must fail at link time rather than silently
// run a receiver that cannot tune or hold its gain. lmac_stop_hw_txq lives in
// libpp.a, phy_track_pll_deinit in libesp_phy.a.
// ------------------------------------------------------------
extern "C" {
    void phy_set_freq(uint16_t freq_mhz, int offset);
    void phy_force_rx_gain(bool enable, uint8_t gain_idx);
    void phy_disable_agc(void);
    void phy_rfagc_disable(void);
    void phy_wifi_fbw_sel(uint32_t val);
    void phy_chip_set_chan_offset(int offset_khz);
    int  phy_get_rssi(void);
    int  phy_get_noise_floor(void);
    void phy_track_pll_deinit(void);
    int  lmac_stop_hw_txq(void);
}

// ------------------------------------------------------------
// Registers (ESP32-C5). Addresses are C5VRX's hardware findings.
// ------------------------------------------------------------
#define REG32(a)         (*(volatile uint32_t *)(uintptr_t)(a))

// MAC TX hardware queues: TXQ0 config, five queues at a -0x10 stride
#define MAC_TXQ0_CONF    0x600a4d6cu
#define MAC_TXQ_STRIDE   0x10u
#define MAC_TXQ_ENABLE   0x80000000u
#define MAC_TXQ_COUNT    5u

// Modem front-end un-gating and the RF dump engine that feeds MODEM_DIAG
#define DUMP_CTRL        0x600a9004u
#define DUMP_PTR_MODE    0x600a9008u
#define DUMP_FORMAT      0x600a9018u
#define FE_PATH          0x600a20b4u
#define FE_ENABLE        0x600a0800u
#define SOURCE_CTRL      0x600a08ccu
#define SOURCE_MUX       0x600a70b8u
#define MODEM_CLOCK      0x600a9c04u
#define CTRL_ENABLE      0x80000000u
#define CTRL_DUMP_FIRST  0x00020000u
#define TX_START_SELECT  0x00060000u
#define SELECTOR_MASK    0x01fe0000u
#define HP_SRAM_USAGE    0x60095004u

static const int     k_iq_gpio[IQ_LANE_COUNT] = IQ_LANE_GPIOS;
static const uint8_t k_iq_diag[IQ_LANE_COUNT] = IQ_LANE_DIAG;

static RfStatus    g_st         = {};
static char        g_err[96]    = "";
static bool        g_bw40       = (RF_BW40 != 0);

static inline void fence() { __asm__ __volatile__("fence iorw, iorw" ::: "memory"); }

static void set_err(const char* what, esp_err_t e) {
    snprintf(g_err, sizeof(g_err), "%s: %s", what, esp_err_to_name(e));
}

// Public 20 MHz 5 GHz centres the closed PHY accepts as a bootstrap. The
// regulatory table decides which of them esp_wifi_set_channel() allows.
struct Wifi5Centre { uint8_t ch; uint16_t mhz; };
static const Wifi5Centre k_centres[] = {
    { 36, 5180}, { 40, 5200}, { 44, 5220}, { 48, 5240},
    { 52, 5260}, { 56, 5280}, { 60, 5300}, { 64, 5320},
    {100, 5500}, {104, 5520}, {108, 5540}, {112, 5560},
    {116, 5580}, {120, 5600}, {124, 5620}, {128, 5640},
    {132, 5660}, {136, 5680}, {140, 5700}, {144, 5720},
    {149, 5745}, {153, 5765}, {157, 5785}, {161, 5805},
    {165, 5825}, {169, 5845}, {173, 5865}, {177, 5885},
};
#define N_CENTRES (sizeof(k_centres) / sizeof(k_centres[0]))

// ------------------------------------------------------------
// Receive-only: hardware-disable the five LMAC transmit queues
// ------------------------------------------------------------
static esp_err_t lock_rx_only() {
    (void)lmac_stop_hw_txq();
    for (unsigned q = 0; q < MAC_TXQ_COUNT; q++)
        REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) &= ~MAC_TXQ_ENABLE;
    fence();
    for (unsigned q = 0; q < MAC_TXQ_COUNT; q++) {
        if (REG32(MAC_TXQ0_CONF - q * MAC_TXQ_STRIDE) & MAC_TXQ_ENABLE)
            return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

// ------------------------------------------------------------
// MODEM_DIAG lanes -> GPIO matrix. The pads are configured input+output so
// the diagnostic bit drives the pad and PARLIO reads the same pad back.
// ------------------------------------------------------------
void rf_route_iq_lanes() {
    uint64_t mask = 0;
    for (unsigned i = 0; i < IQ_LANE_COUNT; i++) mask |= 1ULL << k_iq_gpio[i];
    gpio_config_t cfg = {};
    cfg.pin_bit_mask  = mask;
    cfg.mode          = GPIO_MODE_INPUT_OUTPUT;
    cfg.pull_up_en    = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type     = GPIO_INTR_DISABLE;
    (void)gpio_config(&cfg);
    for (unsigned i = 0; i < IQ_LANE_COUNT; i++)
        esp_rom_gpio_connect_out_signal((gpio_num_t)k_iq_gpio[i], MODEM_DIAG0_IDX + k_iq_diag[i], false, false);
    fence();
}

// ------------------------------------------------------------
// Keep the modem ADC clocking and the RF dump engine streaming pre-trigger
// samples onto MODEM_DIAG with no Wi-Fi packet present. Re-run after every
// retune (the closed driver touches this state).
// ------------------------------------------------------------
static void enable_continuous_modem() {
    // CPU keeps ownership of HP SRAM
    REG32(HP_SRAM_USAGE) = (REG32(HP_SRAM_USAGE) & 0xfffef0ffu) | 0x00010000u;

    // Un-gate modem clocks, force the front end active
    REG32(SOURCE_CTRL) &= 0xff87ffffu;
    REG32(SOURCE_MUX)   = (REG32(SOURCE_MUX) & 0xfffffff8u) | 1u;
    REG32(MODEM_CLOCK)  = UINT32_MAX;
    REG32(FE_ENABLE)   |= 4u;
    REG32(FE_PATH)     &= ~1u;

    // DUMP_FORMAT mode 0 (C5VRX "golden" RF dump configuration)
    uint32_t v = REG32(DUMP_FORMAT);
    v = (v & 0xff03ffffu) | 0x006c0000u;             REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xfffc0fffu) | 0x0001a000u; REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xfffff03fu) | 0x00000640u; REG32(DUMP_FORMAT) = v;
    v = (REG32(DUMP_FORMAT) & 0xffffffc0u) | 0x18u;       REG32(DUMP_FORMAT) = v | 0x01000000u;

    // TX_START selector in pre-trigger circular mode. With the TX queues
    // quiescent TX_START never fires, so with DUMP_FIRST the engine streams
    // pre-trigger samples forever. The software trigger bits stay masked.
    REG32(DUMP_PTR_MODE) = (REG32(DUMP_PTR_MODE) & ~SELECTOR_MASK) | TX_START_SELECT;

    uint32_t ctrl = REG32(DUMP_CTRL);
    ctrl &= ~(CTRL_ENABLE | 0x00080000u | 0x00040000u);   // clear ENABLE, START, DONE
    ctrl |= CTRL_DUMP_FIRST;
    ctrl  = (ctrl & ~0x0001ffffu) | 16384u;               // length
    REG32(DUMP_CTRL) = ctrl;
    fence();
    REG32(DUMP_CTRL) = ctrl | CTRL_ENABLE;
    fence();
}

// Re-assert the analog receive state the closed driver may have disturbed.
static void reassert_rx_state() {
    phy_disable_agc();
    phy_rfagc_disable();
    phy_wifi_fbw_sel(g_bw40 ? 1u : 0u);
    phy_force_rx_gain(true, g_st.gain);
}

// ------------------------------------------------------------
bool rf_start() {
    g_st = {};
    g_st.gain = RF_GAIN_MAX;
    g_st.bw40 = g_bw40;
    esp_err_t err;

    // Arduino's core has already done nvs_flash_init(); the Wi-Fi driver
    // also needs a netif and the default event loop (tolerate "already").
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { set_err("esp_netif_init", err); return false; }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { set_err("esp_event_loop_create_default", err); return false; }

    // Our own esp_wifi_init: the Arduino WiFi class is never used, because it
    // leaves sta_disconnected_pm on, which periodically powers the PHY down
    // and stops the I/Q stream.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.sta_disconnected_pm = false;
    if ((err = esp_wifi_init(&cfg)) != ESP_OK)                 { set_err("esp_wifi_init", err); return false; }
    if ((err = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK) { set_err("esp_wifi_set_storage", err); return false; }
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK)   { set_err("esp_wifi_set_mode", err); return false; }
    if ((err = esp_wifi_start()) != ESP_OK)                    { set_err("esp_wifi_start", err); return false; }

    if ((err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)) != ESP_OK) { set_err("esp_wifi_set_band_mode", err); return false; }
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK)       { set_err("esp_wifi_set_ps", err); return false; }

    // Regulatory domain. The default ("01", world-safe) is what C5VRX ran on;
    // it bounds which public 5 GHz centres esp_wifi_set_channel() accepts and
    // therefore which FPV channels can be reached. Nodes are receive-only, so
    // this carries no transmit implications. Set RF_COUNTRY_CC in config.h if
    // the heartbeat's tune_fail count is not zero.
    if (RF_COUNTRY_CC[0]) {
        wifi_country_t ctry = {};
        strncpy(ctry.cc, RF_COUNTRY_CC, sizeof(ctry.cc) - 1);
        ctry.schan = 1;
        ctry.nchan = 13;
        ctry.max_tx_power = 20;          // unused (receive-only) but must be valid
        ctry.policy = WIFI_COUNTRY_POLICY_MANUAL;
        ctry.wifi_5g_channel_mask = 0;   // 0 = the country's own 5 GHz rules
        if ((err = esp_wifi_set_country(&ctry)) != ESP_OK) { set_err("esp_wifi_set_country", err); return false; }
    }

    wifi_protocols_t protocols = {};
    protocols.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
    protocols.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N;
    if ((err = esp_wifi_set_protocols(WIFI_IF_STA, &protocols)) != ESP_OK) { set_err("esp_wifi_set_protocols", err); return false; }

    // BW40 is a hard requirement for MODEM_DIAG I/Q precision. No fallback.
    wifi_bandwidths_t bw = {};
    bw.ghz_2g = WIFI_BW20;
    bw.ghz_5g = WIFI_BW40;
    if ((err = esp_wifi_set_bandwidths(WIFI_IF_STA, &bw)) != ESP_OK) { set_err("esp_wifi_set_bandwidths(BW40)", err); return false; }

    // First bootstrap centre: 5865 MHz (ch 173, = FPV A1). Fall back through
    // the table if the regulatory domain refuses it.
    bool tuned = false;
    for (int i = (int)N_CENTRES - 1; i >= 0 && !tuned; i--) {
        if (esp_wifi_set_channel(k_centres[i].ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) continue;
        uint8_t p = 0; wifi_second_chan_t s = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&p, &s) == ESP_OK && p == k_centres[i].ch) {
            g_st.wifi_channel = p;
            g_st.tuned_mhz    = k_centres[i].mhz;
            tuned = true;
        }
    }
    if (!tuned) { set_err("no 5 GHz channel accepted by regulatory table", ESP_ERR_NOT_SUPPORTED); return false; }

    // Promiscuous with an empty filter keeps the RX path and MODEM_DIAG alive
    // without the MAC buffering packets or raising interrupts.
    if ((err = esp_wifi_set_promiscuous(true)) != ESP_OK) { set_err("esp_wifi_set_promiscuous", err); return false; }
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = 0;
    (void)esp_wifi_set_promiscuous_filter(&filter);

    if ((err = lock_rx_only()) != ESP_OK) { set_err("MAC TX queues still enabled", err); return false; }

    rf_route_iq_lanes();
    enable_continuous_modem();

    // Vendor packet AGC out of the loop; fixed gain from here on.
    reassert_rx_state();

    // Stop the periodic PLL/RX calibration timer: it would recalibrate the
    // receive chain once a second under us.
    phy_track_pll_deinit();

    g_st.started = true;
    g_err[0] = 0;
    return true;
}

// ------------------------------------------------------------
bool rf_tune(uint16_t freq_mhz) {
    // Try public centres nearest first; a rejected one means the regulatory
    // table forbids it, so move to the next nearest.
    uint8_t order[N_CENTRES];
    for (unsigned i = 0; i < N_CENTRES; i++) order[i] = (uint8_t)i;
    for (unsigned a = 1; a < N_CENTRES; a++) {           // insertion sort by |delta|
        uint8_t k = order[a];
        int dk = abs((int)k_centres[k].mhz - (int)freq_mhz);
        int b = (int)a - 1;
        while (b >= 0 && abs((int)k_centres[order[b]].mhz - (int)freq_mhz) > dk) { order[b + 1] = order[b]; b--; }
        order[b + 1] = k;
    }

    bool ok = false;
    uint8_t  ch = 0;
    uint16_t centre = 0;
    for (unsigned n = 0; n < 4 && !ok; n++) {           // nearest four are enough
        const Wifi5Centre& c = k_centres[order[n]];
        if (abs((int)c.mhz - (int)freq_mhz) > 160) break;   // never bootstrap from another band
        if (esp_wifi_set_channel(c.ch, WIFI_SECOND_CHAN_NONE) != ESP_OK) continue;
        uint8_t p = 0; wifi_second_chan_t s = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&p, &s) != ESP_OK || p != c.ch) continue;
        ok = true; ch = c.ch; centre = c.mhz;
    }
    if (!ok) return false;

    // Exact FPV frequency from the nearest public centre (undocumented call;
    // starting from the nearest centre keeps the step small).
    if (freq_mhz != centre) phy_set_freq(freq_mhz, 0);

    enable_continuous_modem();
    reassert_rx_state();

    g_st.wifi_channel = ch;
    g_st.tuned_mhz    = freq_mhz;
    return true;
}

void rf_set_gain(uint8_t idx) {
    if (idx < RF_GAIN_MIN) idx = RF_GAIN_MIN;
    if (idx > RF_GAIN_MAX) idx = RF_GAIN_MAX;
    if (idx == g_st.gain) return;
    g_st.gain = idx;
    phy_force_rx_gain(true, idx);
}

uint8_t rf_gain() { return g_st.gain; }

void rf_set_bw40(bool bw40) {
    g_bw40    = bw40;
    g_st.bw40 = bw40;
    phy_wifi_fbw_sel(bw40 ? 1u : 0u);
}

bool rf_phy_rssi_dbm(int* dbm) {
    int v = phy_get_rssi();
    if (v < -140 || v > 10) return false;
    *dbm = v;
    return true;
}

bool rf_phy_noise_dbm(int* dbm) {
    int v = phy_get_noise_floor();
    if (v < -140 || v > -20) return false;
    *dbm = v;
    return true;
}

const RfStatus& rf_status()     { return g_st; }
const char*     rf_last_error() { return g_err; }
