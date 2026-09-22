# Level 1 station v3 prototype — the XIAO ESP32-C5 as the 5.8 GHz receiver

Status: **firmware implemented and building; nothing bench validated.** This is
the build spec for the v3 station and the record of what is and is not proven.
The firmware in `level1-c5phy/` implements it. The v2 station it replaces is
documented in [`LEVEL1-V2-HARDWARE.md`](LEVEL1-V2-HARDWARE.md); everything not
mentioned here (enclosure, patches, switch, solar power, Heltec) is unchanged
from v2.

The idea, from the [C5VRX](https://github.com/colonelpanichacks/c5vrx) project:
the ESP32-C5's 5 GHz Wi-Fi radio covers 5.15–5.9 GHz with a 40 MHz filter, its
synthesizer can be told any frequency in MHz, and its modem exposes raw I/Q
samples on a diagnostic bus that another peripheral can read. Analog FPV video
is wideband FM around 5.6–5.95 GHz. So the C5 already contains a 5.8 GHz FM
receiver; the RX5808 and the sync separator only need replacing with software.

```
  4 patches ──► SP4T ──► U.FL ──► ESP32-C5 Wi-Fi PHY (5 GHz, BW40, fixed gain)
                 ▲                      │ MODEM_DIAG: 4-bit I + 4-bit Q, 80 MS/s
        D2/D8/D9 │                      ▼
                 │              GPIO matrix: 8 lanes out to pads, back in
                 │                      ▼
                 │              PARLIO RX ── DMA ──► 16 KiB window @ 40 MS/s
                 │                      ▼
                 └──────── firmware: power/coherence ► level dB ► dBm per sector
                                     FM demod ► H-sync search ► PAL/NTSC, fp
                                     ▼
                           USB JSON  |  D4/D5 UART ► Heltec ► mesh
```

## 1. Parts list

Per station. v2 items that vanish are listed at the end.

### RF front end

| # | Part | Qty | Notes | ~Cost |
|---|------|-----|-------|-------|
| 1 | **Seeed XIAO ESP32-C5** | 1 | Receiver **and** MCU. Only the U.FL connector is used for RF; the antenna it ships with is not | $10 |
| 2 | **5.8 GHz patch antenna, 8 dBi, ~70° beamwidth, SMA** | 4 | As v2: one batch, matched; linear or RHCP, never mixed on one station | $8 ea |
| 3 | **SP4T RF switch, 0.1–6 GHz, 3.3 V CMOS control** | 1 | SKY13322-375LF or PE42442, as v2. Common port → the XIAO U.FL through a short u.FL pigtail | $3 IC / ~$40 EVB |
| 4 | u.FL ↔ SMA pigtails, **equal length, ≤ 15 cm** | 4 | Patches to switch | $2 ea |
| 5 | u.FL ↔ u.FL pigtail, ≤ 5 cm | 1 | Switch common to the XIAO | $2 |
| 6 | SMA bulkhead feedthroughs | 4 | One per enclosure face | $2 ea |
| 7 | 5.8 GHz band-pass filter (optional) | 1 | Between switch and XIAO if a nearby 5 GHz Wi-Fi AP overloads the front end. The PHY's own filter is 40 MHz wide, so a strong AP on an adjacent Wi-Fi channel does raise the measured level (not the coherence) | $5 |

### MCU and mesh

| # | Part | Qty | Notes | ~Cost |
|---|------|-----|-------|-------|
| 8 | **Heltec WiFi LoRa 32 V3** + 868/915 MHz antenna | 1 | Meshtastic, serial module TEXTMSG 115200, node name = `NODE_ID` | $20 |
| 9 | 2 kΩ series resistors on the switch control lines; 100 nF decoupling | — | | $1 |

### Power (unattended solar) and mechanical

Unchanged from v2 §1: 20 W panel, MPPT charger with 5 V out, 4 × 18650, IP65
box with the patches on four faces, brackets, glands.

### Removed from the v2 BOM

RX5808 ($8), sync separator LMH1980/LM1881 ($3), video coupling capacitor and
75 Ω termination, the 10 µF on the RX5808 supply, the RX5808 u.FL pigtail. About
**$12 and three ICs per station less**, and no video wiring.

Total per station ≈ **$175** including panel and battery; ≈ $80 for the
electronics alone.

## 2. Pin budget

The firmware takes D-label GPIO numbers from the Arduino core's `XIAO_ESP32C5`
variant (`pins_arduino.h`). The I/Q loopback lanes are GPIO numbers because the
set is the one C5VRX proved.

| XIAO pin | C5 GPIO | Use |
|----------|---------|-----|
| D0 | 1 | I/Q lane Q9 — **unconnected** |
| D1 | 0 | I/Q lane Q8 — **unconnected** |
| D2 | 25 | switch select V1 |
| D3 | 7 | I/Q lane Q6 — **unconnected** |
| D4 | 23 | Heltec RX (UART TX) |
| D5 | 24 | Heltec TX (UART RX) |
| D6 | 11 | free (UART0 TX; ROM/IDF boot messages appear here) |
| D7 | 12 | free (UART0 RX) |
| D8 | 8 | switch select V2 |
| D9 | 9 | switch select V3 (unused if the switch decodes 2 lines) |
| D10 | 10 | I/Q lane I9 — **unconnected** |
| back pad | 2 | I/Q lane Q7 — **unconnected** |
| back pad | 3 | I/Q lane I7 — **unconnected** |
| back pad | 4 | I/Q lane I6 — **unconnected** |
| back pad | 5 | I/Q lane I8 — **unconnected** |
| — | 27 | on-board LED (active low): on = hit this sweep, blinking = PHY failed |
| — | 6 / 26 | battery sense / enable (variant); not used |

**Why the lanes need pads at all.** The modem's diagnostic bits are peripheral
output signals; PARLIO's data inputs are peripheral input signals. The GPIO
matrix connects a peripheral output to a pad and a pad to a peripheral input,
so the bits travel out through the pad driver and back in through the pad's
input buffer. Nothing leaves the board, but the pad is part of the path: a
resistor or a probe on it corrupts the samples. A carrier PCB must not route
these pads anywhere.

**Why D2/D8/D9 for the switch.** D1/D3 (v2's V1/V3) are lanes. D6/D7 are
UART0, which the ROM bootloader and the IDF console drive at boot; harmless as
GPIO but they chatter during reset, so they are left as a debug console instead.

**D4/D5.** Same header pins as every station tier. C5VRX measured their resistor
DAC on the XIAO's D4–D9 pads as GPIO 23, 24, 11, 12, 8, 9 — the variant numbers.
That settles the C5 D4/D5 question this branch carried since v2.

## 3. How the receiver works

Implemented in `level1-c5phy/src/c5phy_rf.*`, `iq_capture.*`, `demod.*`.

### 3.1 Bringing the PHY up receive-only (`rf_start`)

1. `esp_wifi_init` with `sta_disconnected_pm = false` (the default periodically
   powers the PHY down while disconnected and the I/Q stream stops), station
   mode, RAM storage, start.
2. `WIFI_BAND_MODE_5G_ONLY`, no power saving, 802.11a/n on 5 GHz, **BW40** on
   5 GHz (a hard requirement: C5VRX found BW20 degrades the diagnostic I/Q at
   boot; there is no fallback).
3. Park on a public 5 GHz centre (5885 or 5865 MHz first), verify with
   `esp_wifi_get_channel`.
4. Promiscuous mode with an **empty filter**: keeps the receive path and the
   diagnostic bus alive without the MAC buffering packets or raising interrupts.
5. **Disable the five MAC transmit queues** in hardware (`lmac_stop_hw_txq()`,
   then clear the enable bit on each queue register) and verify. From here on
   the radio cannot transmit.
6. Route the eight MODEM_DIAG lanes to the GPIO pads (§2).
7. Un-gate the modem clocks and arm the RF dump engine in "pre-trigger,
   circular, dump-first" mode with a TX_START trigger that can never fire
   because the TX queues are dead: the engine then streams samples onto the
   diagnostic bus forever. This is the register sequence C5VRX measured
   (79.97 MS/s writer, thousands of wraps, zero triggers).
8. Vendor packet AGC off (`phy_disable_agc`, `phy_rfagc_disable`), BW40 filter
   (`phy_wifi_fbw_sel(1)`), **fixed receive gain** (`phy_force_rx_gain(true, 62)`),
   and the PHY's 1 Hz PLL-tracking timer stopped (`phy_track_pll_deinit`) so it
   cannot recalibrate the receive chain underneath a measurement.

### 3.2 Tuning (`rf_tune`)

Each FPV channel is reached in two steps: `esp_wifi_set_channel()` to the
nearest public 20 MHz centre the regulatory table accepts (tried nearest-first,
verified), then `phy_set_freq(freq_mhz, 0)` to the exact frequency. Band A
(5745–5865 in 20 MHz steps) lands exactly on Wi-Fi channels 149–173 and needs no
second step. After every retune the dump engine is re-armed and the AGC-off /
BW40 / fixed-gain state re-asserted, because the closed driver touches it.

A channel that no nearby centre can bootstrap is marked untunable and counted
in the heartbeat's `tune_fail`; the count is retried every 32 sweeps.

### 3.3 Capturing I/Q (`iq_capture`)

PARLIO RX, 8 data lines, internal PLL_F240M clock divided to **40 MHz**,
positive sample edge, LSB packing, soft delimiter. That samples every second
80 MS/s modem sample — the configuration C5VRX proved bit-exact against the
modem's own ring. Each capture is a finite, blocking, 16 384-byte DMA transfer
(409.6 µs), one byte per sample: I in the high nibble, Q in the low nibble,
4-bit two's complement. Finite captures (rather than C5VRX's continuous ring)
keep the DMA idle while the PHY is retuned or the gain changed, and every
window is contiguous.

### 3.4 Metrics (`iq_metrics`)

Per window:

| Metric | Definition | Used for |
|---|---|---|
| `p_mean` | mean I²+Q² | level |
| `q_phase` | % of samples with power ≥ 8 and phase step within ±45° of the previous sample | the FM-carrier test; Wi-Fi/OFDM and noise score low |
| `clip` | ‰ of samples with I or Q at full scale | gain stepping |
| `cfo_khz` | mean phase step of the coherent samples × 40 MHz / 2π | carrier offset → `freq_peak`, off-channel rejection |
| `noise_like` | variance/mean² of the power ≥ 0.7 when strong | envelope test (FM is constant-envelope) |
| `modulated` | variance of the phase step > 25 | video swing vs bare CW carrier |

**Level.** With the gain forced and the packet AGC off, power scales with the
input: `level_db = (62 − gain) × 1 dB + 10·log10(p_mean / 2.0)`, where 2.0 is
the mean I²+Q² of noise at gain 62 (C5VRX's measured `NOISE_POWER`) and 1 dB
per gain index is their measured slope. Each sector starts at gain 62; while
`clip` exceeds 30 ‰ the gain steps down 12 and the window is retaken, so strong
carriers are measured unclipped and weak ones at full gain. Dynamic range is
about 60 dB of gain plus ~20 dB inside a window. The level of a sector is the
**minimum** over its windows (a Wi-Fi burst lifts one window, a continuous
carrier lifts all), coherence the median.

**dBm.** `rssi_dbm = RSSI_CAL_DBM_AT_NOISE + level_db × RSSI_CAL_SLOPE + RSSI_CAL_OFFSET_DB`.
The default −95 dBm at level 0 is kTB over ~20 MHz plus a ~6 dB noise figure —
an estimate, to be replaced by the attenuator measurement (README, Calibration).

**Hit.** Strongest sector's `level_db ≥ DETECT_LEVEL_DB` (8) and
`q_phase ≥ DETECT_Q_PHASE_MIN` (40). The coherence gate is the channel
selectivity: a carrier 10 MHz off the tuned frequency steps 90° per sample and
never counts as coherent, so an adjacent FPV channel inside the 40 MHz filter
does not produce a hit on the wrong channel, only a raised level.

### 3.5 Video check (`video_window`)

Software FM discriminator: the angle of every sample from a 256-entry table,
the wrapped difference between consecutive angles is the instantaneous
frequency (6.4 units per MHz), sums over 16-sample blocks (0.4 µs) low-pass it.
Then, in one window:

1. Sync level = mean of the lowest 5 % of blocks; white end = 98th percentile.
   A swing under 4 units/sample is "no video".
2. Threshold at sync + 20 % of the swing; runs of blocks below it are pulses.
3. Pulses 3.2–7.2 µs long are horizontal-sync candidates. Their falling edge is
   refined to sub-sample precision with a 16-sample running sum, and the
   rising edge gives the width.
4. Consecutive candidates 62–65.5 µs apart give the line period. Mean period
   > 63.8 µs → PAL (64.0), else NTSC (63.556).
5. Score: width closeness to 4.7 µs (0–40) plus period closeness to nominal
   (0–60). A window counts as video with ≥ 2 periods and score ≥ 70.

Eight windows spaced 5 ms are demodulated per peak (about 100 ms); the carrier
is video when three or more count, the standard is the majority, `sync_hz` is
40 MHz / mean period, `sync_q` the share of windows that counted. The **field
rate is not measured** (a 410 µs window sees no vertical interval); `field_hz`
is the nominal 50/60 of the detected standard.

### 3.6 Per channel visit

```
rf_tune(channel); delay(RF_TUNE_SETTLE_MS)               ~10 ms
for sector in N, E, S, W:
    select(sector); delayMicroseconds(SECTOR_SETTLE_US)
    gain = 62
    for w in windows (3, or 1 on a quiet channel):
        capture 16 KiB (0.4 ms); metrics (~2 ms)
        if clipping: gain -= 12; settle 3 ms; retake
    level[sector] = min over windows, q = median
best = argmax level
hit = level[best] >= 8 dB and q[best] >= 40 %
```

Sweep time ≈ 40 × (10 + 4 × 8 ms) ≈ 1.7 s at the full window count; quiet
channels drop to one window per sector. After the sweep the strongest due
peaks each get the 8-window video check.

## 4. Power budget

| Load | Current | Power | Notes |
|------|---------|-------|-------|
| XIAO ESP32-C5, Wi-Fi PHY receiving continuously at 240 MHz | ~120 mA @ 3.3 V | ~0.45 W from 5 V (LDO) | The radio never sleeps and never transmits; PARLIO/DMA add little |
| SP4T switch | < 5 mA | 0.02 W | |
| Heltec V3, Meshtastic, mostly RX | ~80 mA @ 5 V | 0.40 W | + ~120 mA during LoRa TX bursts |
| **Station total** | | **≈ 0.9–1.0 W ≈ 23 Wh/day** | v2: 1.3 W |

The v2 solar sizing (20 W panel, 50 Wh battery) carries this with margin. The
C5 figure is an estimate from the C5 datasheet's receive current; measure it.

## 5. Build sequence

1. **Bare XIAO on the bench.** Stock antenna on the U.FL, USB, `h A1`, key a VTX
   on A1. This is checks 1–3 of the README's bench validation and costs nothing
   but the XIAO.
2. **Both video standards.** Check 4. If PAL fails and NTSC passes, the sync
   search needs tuning against real PAL captures; report the `v` output.
3. **Sensitivity.** Check 5, against a v2 station. Decide v3's role from the number.
4. **Switch and patches.** Eval board first; truth table, insertion loss through
   the U.FL, bearing sweep — as v2 §5 steps 1 and 3.
5. **Carrier PCB.** XIAO C5 footprint with D0/D1/D3/D10 and the back pads
   **unrouted**, SP4T with four u.FL launches and a u.FL to the XIAO, control
   lines on D2/D8/D9, UART header on D4/D5, decoupling. No other parts.
6. **Field pair.** Two v3 stations 300–500 m apart plus the existing v1/v2
   stations, same acceptance as v2 §5 step 5.

## 6. Open questions and known risks

- **Sensitivity is unmeasured.** No published dBm-versus-distance figure exists
  for the C5 receiving analog video. The RX5808 is a purpose-built receiver at
  about −90 dBm; the PHY's noise figure is comparable, but its 40 MHz filter
  admits more noise and adjacent-channel power than the RX5808's IF, and the
  4-bit I/Q quantises weak signals. The level formula's slope and noise
  constant are C5VRX's measurements at 5865 MHz, not this station's.
- **PAL is unproven.** C5VRX recovered NTSC video on hardware; their PAL
  diagnostics are still open. The line-period test here is symmetric, but has
  never seen a PAL capture.
- **Channel coverage** depends on the regulatory table: the low E/R channels
  bootstrap from DFS centres (5660–5720 MHz) and the top of E/R from 5885 MHz.
  `tune_fail` in the heartbeat is the measurement; `RF_COUNTRY_CC` the knob.
- **Undocumented PHY calls and raw registers** (`phy_set_freq`, the dump-engine
  registers) are tied to this framework's PHY blob. The platform is pinned and
  the symbols are strong references, so a mismatch fails at link time, but a
  blob that keeps the names and changes the behaviour would only show on the
  bench. C5VRX ran on ESP-IDF 6.0; this is 5.5.
- **The 1 Hz PLL tracking timer** is stopped with `phy_track_pll_deinit`. If the
  Arduino framework ever re-arms it, expect one disturbed window per second.
- **Full erase before flashing.** Stale PHY calibration data in flash left
  C5VRX's receiver deaf after reflashes; the README makes the erase mandatory.
- **Adjacent Wi-Fi.** A 5 GHz AP on an overlapping channel raises the measured
  level on FPV channels inside its 40 MHz footprint (not their coherence).
  `DETECT_LEVEL_DB` and, at worst, the optional band-pass filter are the answers.
- **Antenna path.** The XIAO ESP32-C5 has only the U.FL connector for its
  antenna, so the SP4T feeds it directly; confirm on the first board that no
  on-board antenna switch needs a GPIO to select the external path (the Arduino
  variant defines none).
- **3.3 GHz.** The C5's radio does not reach it; the RX3364 plan stays a
  v2-style module.

## 7. Bench log

Blank until a v3 board is built. Record here, per check in the README's
validation list: date, board, firmware commit, VTX and camera used, and the
numbers (`level_db` at each attenuation, `line_hz` per standard, `tune_fail`,
sweep time from `sweeps` in consecutive heartbeats).
