# RX3364 (3.3 GHz) integration plan — Level 1 stations

Status: **planning**. No RX3364 code exists yet. This document is the spec the
driver work will be done against; the bench results from Gate 0 get recorded in
[§9](#9-bench-log) before any driver code lands.

## 1. Why a second receiver

The RX5808 covers the 5.8 GHz analog video band. Long-range analog FPV has moved a
large share of traffic to **3.3 GHz** (and 1.2/1.3 GHz), where the same 100 mW to 4 W
VTX gets several times the range and where 5.8 GHz-only detection sees nothing. A
Level 1 station that only sweeps 5.8 GHz therefore has a hole exactly where the
longer-range, harder-to-spot aircraft sit. The RX3364 is the cheap, widely stocked
3.3 GHz counterpart of the RX5808 and is the natural next module.

Level 1 stations stay receive-only; nothing here transmits.

## 2. What is known about the RX3364

Compiled from vendor listings (flymod, eBay, Amazon, AliExpress). Treat every row
as "claimed by a seller" until Gate 0 confirms it on the bench.

| Item | Value | Confidence |
|------|-------|------------|
| Frequency range | 3060–3500 MHz | high (all listings agree) |
| Demodulation | FM, PLL synthesizer | high |
| IF | 480 MHz | medium (flymod spec sheet) |
| Sensitivity | −93 to −95 dBm ± 2 | high |
| Supply | **5.0 V**, 30–380 mA | high |
| Video out | 1 V p-p into 75 Ω, ±5 dB 50 Hz–6 MHz, SNR ≥ 38 dB | high (irrelevant to us) |
| Size | 37 × 25 mm | high |
| **RSSI output** | present on the **RX3364 PRO** only; plain RX3364 has no RSSI pin | high |
| RSSI voltage range | **unknown** | — |
| Tuning interface | **unknown**: listings say "SPI module" for some variants, others are channel-button / LED-blink boards | — |
| Register map / formula | **unknown**; not RTC6715 (that IC is 5.8 GHz only) | — |
| Channel settle time | unknown | — |
| Logic level on control pins | unknown (5 V-supplied board) | — |

**Buy the PRO (RSSI) variant.** Without an RSSI pin the module is useless for
detection; the video output could in principle be sync-detected but that is a much
worse signal for range estimation and a lot more firmware.

### 3.3 GHz FPV channel plan

The de-facto 16-channel plan used by the R3300 / Readytosky / RX3364-class gear:

| Band | CH1 | CH2 | CH3 | CH4 | CH5 | CH6 | CH7 | CH8 |
|------|-----|-----|-----|-----|-----|-----|-----|-----|
| **A** | 3320 | 3345 | 3370 | 3395 | 3420 | 3445 | 3470 | 3495 |
| **B** | 3310 | 3330 | 3355 | 3380 | 3405 | 3430 | 3455 | 3480 |

Spacing is 25 MHz within a band and the two bands interleave, so the 16 channels
cover 3310–3495 MHz at roughly 10–25 MHz steps. An analog FM video carrier is
~18–27 MHz wide, so a VTX on any frequency in that window lights up one or two
adjacent channels. The module's 3060–3500 MHz range means a wider custom sweep is
possible if the tuning interface is SPI (see §5.2).

## 3. Gate 0 — bench characterisation (blocking)

Do this with one RX3364 PRO, a bench supply, a scope or DMM, and a 3.3 GHz VTX
you can key on demand. Record results in §9.

1. **Identify the board variant.** Photograph both sides. Note every labelled pad:
   expect some subset of `5V`, `GND`, `VIDEO`, `AUDIO`, `RSSI`, `CH1..CH3` / `BAND`,
   `SCK`/`CLK`, `MOSI`/`DATA`, `CS`/`LE`, `BTN`.
2. **Tuning interface.** Decide which of these it is:
   - **(a) SPI, RX5808-style** — three pads (DATA/CLK/CS or LE). Best case: the
     existing bit-bang code carries over with a new register formula.
   - **(b) Parallel channel-select pins** — `CH1/CH2/CH3` (+ `BAND`) pulled high/low
     select one of 8/16 channels. Still fully scannable from GPIO, 4 pins instead of 3.
     Settle time is whatever the on-board MCU/PLL takes.
   - **(c) Button-stepped** — a single pushbutton cycles channels. Scannable by
     pulsing the button line, but the firmware cannot know which channel it is on
     without a reset sequence. Least attractive; consider a different board.
3. **Register formula (SPI only).** Sniff the vendor's own controller if the module
   came with one, otherwise brute-force: write candidate N/A values and watch which
   one demodulates the VTX. The RX5808 formula is `f = 2·(32N + A) + 479`; the RX3364
   is a different synthesizer so expect a different reference and IF offset
   (IF = 480 MHz suggests LO = f − 480 or f + 480).
4. **RSSI curve.** Sweep VTX power / attenuation from below sensitivity to strong,
   log RSSI voltage at ~5 dB steps on the tuned channel. Also log RSSI on the
   adjacent channels and with the VTX off (noise floor). Capture:
   - RSSI **min/max voltage**. If max > 3.1 V the ADC input needs a divider
     (ESP32 ADC at `ADC_11db` saturates around 3.1 V and is not 5 V tolerant).
   - Whether the curve is roughly linear in dBm (RX5808 is, over ~70 dB).
5. **Settle time.** Tune away and back while watching RSSI on the scope; measure
   time from the tune command to a stable RSSI. Sets `TUNE_SETTLE_MS` for the
   receiver.
6. **Control-pin logic level.** Confirm DATA/CLK/CS (or CH pins) accept 3.3 V logic
   with the board on 5 V. If they need 5 V, a level shifter or open-drain + pull-up
   goes on the carrier.
7. **Current.** Measure supply current at idle and while tuned to a strong carrier.
   380 mA at 5 V is nearly 2 W and matters for the solar budget of a fielded station.

**Exit criteria:** tuning method, register formula or pin truth table, RSSI voltage
range and curve, settle time, logic level, current, all written into §9.

## 4. Target architecture: one Level 1 firmware, pluggable receivers

**Done (M1).** `level1-analog-fpv/src/main.cpp` is receiver-agnostic and refers only
to the `RX_*` names; the RX5808 driver sits behind `analog_receiver.h` in
`receivers/`. The RX3364 slots in behind the same seam rather than forking `main.cpp`.

### 4.1 Directory layout (as it exists on this branch)

```
level1-analog-fpv/
  platformio.ini                 <- envs: seeed_xiao_esp32s3, seeed_xiao_esp32c5 (-DRECEIVER_RX5808)
                                    RX3364 adds rx3364_s3 / rx3364_c5 with -DRECEIVER_RX3364
  src/
    config.h                     <- every pin and tuneable (pins via the XIAO variant D0..D10)
    main.cpp                     <- sweep × sectors, peak-pick, fine-tune + video, JSON, relay (receiver-agnostic)
    analog_receiver.h            <- the interface every receiver implements
    sector_switch.* bearing.* video_sync.*   <- v2 station modules, receiver-independent
    receivers/
      rx5808.h / rx5808.cpp      <- today's driver
      rx3364.h / rx3364.cpp      <- new
```

`firmware/` gains `level1-v2-rx3364-s3.bin` / `-c5.bin`.

### 4.2 Receiver interface

```cpp
// analog_receiver.h
struct AnalogChannel {
    uint16_t    freq_mhz;
    const char* band;   // "R","A","B","E","F" (5.8G)  |  "A","B" (3.3G)
    uint8_t     ch;     // 1–8
};

// Selected at build time by -DRECEIVER_RX5808 / -DRECEIVER_RX3364 in platformio.ini.
// Exactly one must be defined; #error otherwise.
#if defined(RECEIVER_RX3364)
  #include "receivers/rx3364.h"
#elif defined(RECEIVER_RX5808)
  #include "receivers/rx5808.h"
#else
  #error "Define RECEIVER_RX5808 or RECEIVER_RX3364 in platformio.ini build_flags"
#endif

// Every receivers/*.h provides:
//   #define RX_NAME            "rx5808" | "rx3364"        (JSON "receiver" key)
//   #define RX_BAND_LABEL      "5.8G"   | "3.3G"          (basic_id prefix)
//   #define RX_CHANNEL_COUNT   40       | 16              (#define: sizes stack arrays,
//                                                          static_assert'd against the table)
//   #define RX_TUNE_SETTLE_MS  30       | <Gate 0>
//   extern const AnalogChannel RX_CHANNELS[];
//   void  rx_init();
//   void  rx_tune(uint16_t freq_mhz);
//   void  rx_read_rssi_stats(RssiStats*);  // RSSI_SAMPLES reads, raw + calibrated mV
//   float rx_mv_to_dbm(int mv);            // per-receiver calibration from Gate 0
//   float rx_raw_to_dbm(int raw);
```

This is the interface as implemented in `analog_receiver.h`. The threshold and the
calibration line live in `config.h`, not the driver; an RX3364 build overrides
them there. The RSSI pin stays on D0 for both receivers.

### 4.3 Pin budget

Both receivers use the same D-pin slots so one carrier PCB works for either:

| Signal | XIAO pin | S3 GPIO | C5 GPIO | RX5808 | RX3364 (SPI variant) | RX3364 (CH-pin variant) |
|--------|----------|---------|---------|--------|----------------------|-------------------------|
| DATA / CH1 | D10 | 9 | 10 | DATA | DATA/MOSI | CH1 |
| CLK / CH2 | D8 | 7 | 8 | CLK | CLK | CH2 |
| CS / CH3 | D9 | 8 | 9 | CS | CS/LE | CH3 |
| BAND (CH-pin variant only) | D1 | 2 | 3 | — | — | BAND |
| RSSI | D0 | 1 | 2 | RSSI (0–1 V) | RSSI (via divider if > 3.1 V) | same |
| Heltec TX / RX | D4 / D5 | 5 / 6 | 6 / 7 | ✓ | ✓ | ✓ |
| Power | 5V + GND | | | 3V3 pin | **5V pin** | **5V pin** |

The RX3364 wants 5 V. On the XIAO the `5V` pin is VBUS/battery-boost, so a solar
carrier feeds it from the 5 V rail rather than the 3.3 V LDO. RSSI stays on **D0**
(ADC1 on both boards) so the mapper-side assumptions about which pin is the ADC do
not change. A **dual-receiver station** (RX5808 + RX3364 on one XIAO) fits the
S3/C5 pin count (D0–D10 = 11 GPIO; needs 4 + 4 + 2 UART = 10) but doubles the
sweep time and is a separate milestone (§7, M5); it is not in the first cut.

### 4.4 Scan loop changes

- Sweep time = `RX_CHANNEL_COUNT × (RX_TUNE_SETTLE_MS + MIN_DWELL_HITS × 5 ms)`.
  RX5808: 40 × 40 ms = 1.6 s. RX3364 at 16 channels and a Gate 0 settle of, say,
  50 ms: 16 × 60 ms ≈ 1 s. Either is well inside the 5 s `REPORT_INTERVAL_MS`.
- `last_report_ms[RX_CHANNEL_COUNT]` and the pre-aging loop already scale.
- The heartbeat `channels` field reports `RX_CHANNEL_COUNT`; add `receiver`.
- Startup banner: `{"info":"<RX_NAME> scanner ready", ...}`.
- Optional wide sweep (SPI variant only): a second table stepping 3060–3500 MHz in
  20 MHz steps for "unknown channel plan" VTXs, enabled by `#define WIDE_SWEEP 1`.
  Reports use band `"W"` and the step index as `ch`. Off by default; it triples the
  sweep time.

## 5. Mapper contract changes (land on `main`)

`mesh-mapper.py` currently hard-codes RX5808 physics in three places. All three
are fixed by two **additive** JSON keys, so existing RX5808 stations keep working
unchanged and the mapper degrades gracefully when the keys are absent.

### 5.1 New keys emitted by every Level 1 station

```json
{
  "type":     "analog_fm",
  "receiver": "rx3364",
  "mac":      "AF:00:0C:F8:41:01",
  "freq_mhz": 3320,
  "band":     "A",
  "ch":       1,
  "rssi_raw": 2210,
  "rssi":     2210,
  "rssi_dbm": -71.5,
  "basic_id": "3.3G-A1-3320MHz",
  "node_id":  "RX07"
}
```

| Key | Purpose |
|-----|---------|
| `receiver` | Which driver produced it. Log line, popup, and the only thing the mapper should branch on if a receiver-specific tweak is ever unavoidable. |
| `rssi_dbm` | Received power, converted **in the firmware** from `rssi_raw` using that receiver's Gate 0 curve. The mapper prefers this over its own `_rssi_raw_to_dbm` when present. Moves the ADC-curve knowledge to the one place that knows the hardware. |

The RX5808 driver starts emitting both keys in the same refactor (its curve is the
existing `-85 dBm @ threshold … -15 dBm @ full scale` approximation until someone
measures it properly).

The synthetic MAC scheme (`AF:00:` + freq hi/lo + band ASCII + ch) already keeps
3.3 GHz and 5.8 GHz channels distinct because the frequency bytes differ, so
tracking keys do not collide even though both plans have bands "A" and "B".

### 5.2 Mapper edits

| Where (`mesh-mapper.py` on `main`) | Today | Change |
|---|---|---|
| `_FSPL_5800_DB = 47.72`, `_rssi_to_max_range_m()` | 5.8 GHz constant baked in | Compute `20·log10(freq_mhz) − 27.55` from the detection's `freq_mhz` (5800 → 47.72, unchanged; 3400 → 43.08, i.e. ~1.7× larger ring for the same dBm). |
| `_rssi_raw_to_dbm()` call site in `update_detection()` | always converts `rssi_raw` with the RX5808 curve | Use `rssi_dbm` if the detection carries it; fall back to the RX5808 curve only when it doesn't. |
| TAK `callsign = f"5.8G-{band}{ch}-{freq}MHz"` (`_queue_tak_for_detection`) | prefix hard-coded | Use `basic_id` from the detection (already carries the right prefix). |
| JS `rfRssiToColor(rssi_raw)` thresholds 800/1000 | RX5808 ADC counts | If `det.rssi_dbm` is present colour on dBm bands (e.g. ≥ −60 green, ≥ −75 amber, else red); else keep the count thresholds. |
| `generateAnalogFmPopup()` | no receiver shown | Add `Receiver:` and `dBm:` rows. |
| `[RX5808] Analog FM signal:` log line | fixed tag | `[{receiver or 'analog'}]`. |
| `cleanup_old_detections()` 30 s analog timeout | fine | none |
| `isNoGpsDrone` guards, `_skip_faa`, history skip | keyed on `type == 'analog_fm'` | none; the new receiver reuses the type, which is the point of the contract |

Everything above is a no-op for a detection without the new keys, so the `main`
change can merge before any RX3364 station exists.

## 6. Risks and open questions

1. **Tuning interface is unknown** (Gate 0 §3.2). If it is button-stepped (c),
   pick a different 3.3 GHz module with SPI or CH pins rather than build a fragile
   state-tracking driver.
2. **RSSI voltage > 3.1 V** would need a divider on the carrier PCB; decide the
   ratio from Gate 0 so the S3 and C5 carriers are revised once.
3. **5 V power** on a solar station. 0.4 A peak at 5 V is more than an RX5808 node
   draws in total; confirm the solar/battery sizing before a field build.
4. **Adjacent-channel bleed** at 25 MHz spacing with ~20 MHz-wide carriers: a single
   VTX will light two neighbours. The mapper already tracks each channel as its own
   device, so this shows as 2–3 rings of decreasing RSSI at the same spot. Acceptable
   for v1; a "peak-pick within a band" filter in firmware is an optional M4 item.
5. **Wide (non-standard) channel plans.** Some 3.3 GHz VTXs are user-tunable across
   the whole 3060–3500 MHz range. Only the SPI variant can chase those (§4.4 wide
   sweep).
6. **Antenna.** 3.3 GHz needs its own antenna; the 5.8 GHz whip on the RX5808 is
   badly mismatched there. Budget an SMA/u.FL 3.3 GHz dipole or cloverleaf per
   station.
7. **Image response** with a 480 MHz IF: a strong 5.8 GHz or 2.4 GHz source will
   not alias in, but a carrier ~960 MHz away (2.4 GHz band edge is too far; 3.3 GHz ±
   960 MHz is 2.3–4.4 GHz) might. Check on the bench with a 2.4 GHz Wi-Fi AP close by.

## 7. Milestones

| # | Milestone | Deliverable | Acceptance |
|---|-----------|-------------|------------|
| M0 | Gate 0 bench characterisation | §9 filled in, photos of the module in `docs/rx3364/` | Every unknown in §2 resolved |
| M1 ✅ | Refactor onto `AnalogReceiver` | `level1-analog-fpv/` layout, `receiver` + `rssi_dbm` keys | Done with the v2 station restructure; both boards build |
| M2 ✅ | Mapper contract on `main` | `receiver` / `rssi_dbm` accepted, frequency-derived FSPL, `analog_fusion_test.py` | Done on `main` (differential-RSSI fusion commit) |
| M3 | RX3364 driver | `receivers/rx3364.*`, `rx3364_s3`/`rx3364_c5` envs, prebuilt bins, README wiring table | Bench VTX on A1 reports `3.3G-A1-3320MHz` with the expected RSSI; VTX off reports nothing for 5 min |
| M4 | Field validation | One RX3364 station on the mesh next to an RX5808 station | Both rings appear at the right node position with distinct `basic_id`s; solar node survives 72 h |
| M5 (stretch) | Dual-receiver station | Second driver instance on D1/D2/D3 + second ADC pin | Sweep both bands from one XIAO |

M1 and M2 are independent of the hardware and can start now. M3 is blocked on M0.

## 8. Sources

- flymod RX3364 PRO listing (spec sheet: 3060–3500 MHz, −95 dBm, 480 MHz IF, 5 V,
  30–380 mA, 37×25 mm): https://flymod.net/en/item/receiver_pro_rx3364_rssi
- Readytosky 3.3 GHz VRX (16-channel A/B table): https://flymod.net/en/item/receiver_readytosky_33ghz_vrx
- GetFPV R3300 V2 (same 16-channel table): https://www.getfpv.com/r3300-3-3-ghz-16-channels-receiver-v2.html
- RX3364 PRO with RSSI, eBay listings: https://www.ebay.com/itm/336088919160 , https://www.ebay.com/itm/205744663512
- "RX3364 3.3G 5.8G Wireless FPV SPI Module", AliExpress (the SPI-variant claim): https://www.aliexpress.com/item/1005009016318451.html
- Band comparison background: https://oscarliang.com/fpv-frequency/ , https://blog.unmanned.tech/fpv-frequency-tested/

## 9. Bench log

_Fill in during Gate 0._

| Item | Result | Date / who |
|------|--------|------------|
| Board variant / photo | | |
| Tuning interface (a/b/c) | | |
| Register formula or CH truth table | | |
| RSSI V at noise floor / −90 / −80 / −70 / −60 / −50 / −40 dBm | | |
| RSSI max voltage; divider needed? | | |
| Settle time (ms) | | |
| Control-pin logic level | | |
| Current idle / tuned (mA @ 5 V) | | |
| Adjacent-channel RSSI with VTX on A4 | | |
