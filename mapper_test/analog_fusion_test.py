#!/usr/bin/env python3
"""
Offline test for the Level 1 analog FM path in mesh-mapper.py:

  * legacy detections (rssi_raw only) still get a ring radius
  * compact mesh-relayed lines get basic_id / rssi / rssi_dbm backfilled
  * differential-RSSI multilateration recovers a simulated emitter position
    from several stations with an UNKNOWN transmitter power, including the
    silent-station (coverage overlap) constraint with only two positives

Runs without hardware, serial ports or a browser. mesh-mapper.py is copied
into a temp dir before import so its startup CSV files land there.

    python3 mapper_test/analog_fusion_test.py
"""
import importlib.util
import math
import os
import random
import shutil
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(os.path.dirname(HERE), "mesh-mapper.py")


def load_mapper():
    tmp = tempfile.mkdtemp(prefix="mapper_test_")
    dst = os.path.join(tmp, "mesh_mapper_under_test.py")
    shutil.copy(SRC, dst)
    spec = importlib.util.spec_from_file_location("mesh_mapper_under_test", dst)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# Station grid (~600 m spacing) around a reference point
LAT0, LON0 = 33.4942, -111.9261
STATIONS = {
    "RX01": (0,    0),
    "RX02": (600,  0),
    "RX03": (0,    600),
    "RX04": (600,  600),
    "RX05": (300,  1200),
    "RX06": (-600, 300),
}
RX_THRESHOLD_DBM = -92.0


def enu_to_latlon(x, y):
    return (LAT0 + y / 110540.0,
            LON0 + x / (111320.0 * math.cos(math.radians(LAT0))))


def latlon_to_enu(lat, lon):
    return ((lon - LON0) * 111320.0 * math.cos(math.radians(LAT0)),
            (lat - LAT0) * 110540.0)


def mac_for(freq, band, ch):
    return "AF:00:%02X:%02X:%02X:%02X" % ((freq >> 8) & 0xFF, freq & 0xFF, ord(band), ch)


def simulate_rx_dbm(tx_dbm, ex, ey, sx, sy, n_exp, noise_db, rng):
    d = max(5.0, math.hypot(ex - sx, ey - sy))
    p0 = tx_dbm - (20 * math.log10(5800) - 27.55)      # power at 1 m
    return p0 - 10 * n_exp * math.log10(d) + rng.gauss(0, noise_db)


def setup(mod, station_ids):
    mod.tracked_pairs.clear()
    mod.ANALOG_OBS.clear()
    mod.ANALOG_FIXES.clear()
    mod.NODE_LOCATIONS.clear()
    mod.NODE_STATUS.clear()
    for nid in station_ids:
        x, y = STATIONS[nid]
        lat, lon = enu_to_latlon(x, y)
        mod.NODE_LOCATIONS[nid] = {"lat": lat, "lon": lon, "alt": 0,
                                   "source": "manual", "last_updated": time.time()}
        mod._analog_note_node({"heartbeat": True, "node_id": nid,
                               "threshold_dbm": RX_THRESHOLD_DBM, "receiver": "rx5808"})


def feed_emitter(mod, station_ids, ex, ey, tx_dbm, n_exp=2.3, noise_db=3.0, seed=1,
                 channel_flip=True, channel=None, extra=None,
                 bearing_noise_deg=None, bearing_sigma_deg=15):
    """Feed one report per station that can hear the emitter. Returns the
    station ids that reported.

    channel:            (freq_mhz, band, ch) for every station; default is the
                        R3/B1 alternation below
    extra:              dict merged into every line (v2 keys such as fp)
    bearing_noise_deg:  when set, each line carries a v2 bearing_deg: the true
                        bearing plus this much gaussian noise, with
                        bearing_sigma_deg as the station's claimed 1σ
    """
    rng = random.Random(seed)
    heard = []
    for i, nid in enumerate(station_ids):
        sx, sy = STATIONS[nid]
        dbm = simulate_rx_dbm(tx_dbm, ex, ey, sx, sy, n_exp, noise_db, rng)
        if dbm < RX_THRESHOLD_DBM:
            continue
        if channel:
            freq, band, ch = channel
        else:
            # Alternate stations peak-pick neighbouring channels (5732 R3 / 5733 B1)
            # to exercise the frequency clustering.
            freq, band, ch = (5733, "B", 1) if (channel_flip and i % 2) else (5732, "R", 3)
        raw = int((dbm + 95) / 75 * 650 + 450) * 4095 // 3100
        det = {
            "type": "analog_fm", "mac": mac_for(freq, band, ch),
            "freq_mhz": freq, "band": band, "ch": ch,
            "rssi_raw": raw, "rssi_dbm": round(dbm, 1),
            "rssi_min": raw - 20, "rssi_max": raw + 20,
            "node_id": nid, "seq": 1 + i,
        }
        if extra:
            det.update(extra)
        if bearing_noise_deg is not None:
            true_brg = math.degrees(math.atan2(ex - sx, ey - sy)) % 360.0
            det["bearing_deg"] = int(round(true_brg + rng.gauss(0, bearing_noise_deg))) % 360
            det["bearing_sigma_deg"] = bearing_sigma_deg
        mod.update_detection(det)
        heard.append(nid)
    return heard


def active_fixes(mod):
    return [f for f in mod.ANALOG_FIXES.values() if f["status"] == "active"]


def nearest_fix(mod, ex, ey):
    fixes = active_fixes(mod)
    assert fixes, "no fix produced"
    return min(fixes, key=lambda f: fix_error_m(f, ex, ey))


def best_fix(mod):
    fixes = [f for f in mod.ANALOG_FIXES.values() if f["status"] == "active"]
    assert fixes, "no fix produced"
    return max(fixes, key=lambda f: f["n_nodes"])


def fix_error_m(fix, ex, ey):
    fx, fy = latlon_to_enu(fix["lat"], fix["lon"])
    return math.hypot(fx - ex, fy - ey)


def test_legacy_ring(mod):
    setup(mod, ["RX01"])
    det = {"type": "analog_fm", "mac": mac_for(5658, "R", 1), "freq_mhz": 5658,
           "band": "R", "ch": 1, "rssi_raw": 850, "rssi": 850,
           "basic_id": "5.8G-R1-5658MHz", "node_id": "RX01"}
    mod.update_detection(det)
    d = mod.tracked_pairs[det["mac"]]
    assert d["node_lat"] and d["radius_m"] > 0, d
    assert d["dbm_source"] == "mapper"
    assert -80 < d["rssi_dbm"] < -60, d["rssi_dbm"]      # 850 counts ≈ -72.7 dBm
    assert 50 < d["radius_m"] < 500, d["radius_m"]       # was ~20 km with the old curve
    print(f"  legacy ring: {d['rssi_dbm']} dBm -> {d['radius_m']} m  OK")


def test_compact_line_backfill(mod):
    setup(mod, ["RX01"])
    det = {"type": "analog_fm", "mac": mac_for(5732, "R", 3), "freq_mhz": 5732,
           "band": "R", "ch": 3, "rssi_raw": 900, "rssi_dbm": -66.2,
           "rssi_min": 880, "rssi_max": 930, "node_id": "RX01", "seq": 7}
    mod.update_detection(det)
    d = mod.tracked_pairs[det["mac"]]
    assert d["basic_id"] == "5.8G-R3-5732MHz", d["basic_id"]
    assert d["rssi"] == 900 and d["receiver"] == "rx5808"
    assert d["dbm_source"] == "firmware" and d["rssi_dbm"] == -66.2
    assert mod.NODE_STATUS["RX01"]["last_seen"] > 0
    print("  compact mesh line backfill  OK")


def test_fix_four_stations(mod):
    ids = ["RX01", "RX02", "RX03", "RX04", "RX05", "RX06"]
    setup(mod, ids)
    ex, ey, tx = 350.0, 420.0, 27.0            # 500 mW VTX inside the grid
    heard = feed_emitter(mod, ids, ex, ey, tx)
    fix = best_fix(mod)
    err = fix_error_m(fix, ex, ey)
    print(f"  4+ stations: heard={heard} fix err={err:.0f} m (±{fix['err_m']} m, "
          f"{fix['quality']}) tx_est={fix['tx_dbm_est']} dBm (true {tx})")
    assert fix["n_nodes"] >= 3, fix
    assert len(fix["macs"]) == 2, fix["macs"]     # both channels clustered into one emitter
    assert err < 250, err
    assert fix["bounded"]
    assert abs(fix["tx_dbm_est"] - tx) < 10, fix["tx_dbm_est"]
    # participating rings carry the fix
    for m in fix["macs"]:
        assert mod.tracked_pairs[m]["fix_id"] == fix["fix_id"]


def test_unknown_tx_power(mod):
    """Same geometry, 20 dB weaker transmitter: position must not move much."""
    ids = ["RX01", "RX02", "RX03", "RX04", "RX05", "RX06"]
    setup(mod, ids)
    ex, ey = 350.0, 420.0
    heard = feed_emitter(mod, ids, ex, ey, 22.0, seed=2)
    fix = best_fix(mod)
    err = fix_error_m(fix, ex, ey)
    print(f"  22 dBm emitter: heard={heard} fix err={err:.0f} m (±{fix['err_m']} m, "
          f"{fix['quality']}) tx_est={fix['tx_dbm_est']}")
    assert 3 <= len(heard) < 6, heard
    assert err < 300, err
    assert abs(fix["tx_dbm_est"] - 22.0) < 10


def test_two_positive_plus_silent(mod):
    """Two stations hear it, three alive stations do not: the silent-station
    constraint must still yield a bounded fix on the correct side."""
    ids = ["RX01", "RX02", "RX03", "RX04"]
    setup(mod, ids)
    # 250 mW south of the RX01–RX02 baseline. Its mirror image across that
    # baseline sits inside the square, well within range of the silent
    # northern pair, which is what lets silence break the tie.
    ex, ey, tx = 300.0, -400.0, 24.0
    heard = feed_emitter(mod, ids, ex, ey, tx, noise_db=1.0, seed=3, channel_flip=False)
    assert heard == ["RX01", "RX02"], heard
    fix = best_fix(mod)
    err = fix_error_m(fix, ex, ey)
    print(f"  2 heard {heard} + {fix['n_silent']} silent: fix err={err:.0f} m "
          f"(±{fix['err_m']} m, {fix['quality']}, bounded={fix['bounded']})")
    assert fix["n_silent"] >= 2
    # With two positives the ratio alone is a circle; silence must pull the
    # solution to the far side of the heard pair, away from the silent ones.
    fx, fy = latlon_to_enu(fix["lat"], fix["lon"])
    d_silent = min(math.hypot(fx - STATIONS[n][0], fy - STATIONS[n][1]) for n in fix["silent_nodes"])
    d_heard  = min(math.hypot(fx - STATIONS[n][0], fy - STATIONS[n][1]) for n in heard)
    assert d_heard < d_silent, (d_heard, d_silent)
    # Must land on the emitter's (southern) side of the baseline, not the mirror
    assert fy < 0, fy


def test_single_station_no_fix(mod):
    setup(mod, ["RX01", "RX02"])
    feed_emitter(mod, ["RX01"], 100.0, 50.0, 20.0)
    assert not mod.ANALOG_FIXES, mod.ANALOG_FIXES
    print("  single station: ring only, no fix  OK")


def test_solver_direct(mod):
    """Exercise _analog_solve on its own with a noise-free triangle."""
    obs = []
    ex, ey, p0 = 120.0, 80.0, -30.0
    for sx, sy in ((0, 0), (500, 0), (0, 500), (500, 500)):
        d = math.hypot(ex - sx, ey - sy)
        obs.append((sx, sy, p0 - 10 * 2.3 * math.log10(d), 1.0))
    sol = mod._analog_solve(obs, [], n_exp=2.3)
    err = math.hypot(sol["x"] - ex, sol["y"] - ey)
    print(f"  direct solve: err={err:.1f} m p0={sol['p0_dbm']:.1f} (true {p0}) rms={sol['rms_db']:.2f}")
    assert err < 15, err
    assert abs(sol["p0_dbm"] - p0) < 1.0
    assert mod._analog_solve(obs[:1], [], n_exp=2.3) is None
    assert mod._analog_solve(obs[:2], [], n_exp=2.3) is None
    # Two exact bearings make the two-station case solvable
    brg = [(sx, sy, math.degrees(math.atan2(ex - sx, ey - sy)) % 360.0, 15.0)
           for sx, sy, _, _ in obs[:2]]
    sol2 = mod._analog_solve(obs[:2], [], n_exp=2.3, bearings=brg)
    assert sol2 is not None and sol2["n_bearings"] == 2
    err2 = math.hypot(sol2["x"] - ex, sol2["y"] - ey)
    print(f"  direct solve, 2 stations + 2 bearings: err={err2:.1f} m bounded={sol2['bounded']}")
    assert err2 < 15 and sol2["bounded"], (err2, sol2["bounded"])


def test_cluster_band_plans(mod):
    """_analog_cluster on real FPV channel plans: every channel of a band is
    its own emitter. Neighbour-chaining used to fold all of Band A, B and F
    (19–20 MHz spacing) into one cluster each."""
    def obs(freq, **kw):
        o = {"freq_mhz": freq, "node_id": kw.pop("node", "RX01"), "mac": mac_for(freq, "X", 0),
             "dbm": -70.0, "fp": None}
        if kw:
            o["fp"] = mod._analog_fingerprint(kw)
        return o

    bands = {
        "Raceband": [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917],
        "Band A":   [5725, 5745, 5765, 5785, 5805, 5825, 5845, 5865],
        "Band B":   [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866],
        "Band F":   [5740, 5760, 5780, 5800, 5820, 5840, 5860],
    }
    for name, freqs in bands.items():
        n = len(mod._analog_cluster([obs(f) for f in freqs]))
        assert n == len(freqs), f"{name}: {len(freqs)} channels -> {n} clusters"
    # Neighbouring channels of one carrier still fuse (R3 5732 / B1 5733, F4/A4/R5/B5)
    assert len(mod._analog_cluster([obs(5732), obs(5733, node="RX02")])) == 1
    assert len(mod._analog_cluster([obs(5800), obs(5805, node="RX02"), obs(5806, node="RX03"),
                                    obs(5809, node="RX04")])) == 1
    # Same channel, different video standard: two emitters
    two = mod._analog_cluster([obs(5800, fp="NTSC/15736/5800"),
                               obs(5800, node="RX02", fp="PAL/15630/5800")])
    assert len(two) == 2, two
    # A legacy report (no fingerprint) never splits, and joins the nearest cluster
    three = mod._analog_cluster([obs(5800, fp="NTSC/15736/5800"),
                                 obs(5800, node="RX02", fp="PAL/15630/5800"),
                                 obs(5805, node="RX03")])
    assert len(three) == 2, three
    # Carrier centres a fine-tune step apart, line rates tens of Hz apart: one emitter
    assert len(mod._analog_cluster([obs(5732, fp="NTSC/15736/5734"),
                                    obs(5733, node="RX02", fp="NTSC/15720/5736")])) == 1
    # Two carriers 20 MHz apart resolved by fine-tune even when channels alone would not
    assert len(mod._analog_cluster([obs(5725, fp="NTSC/15736/5725"),
                                    obs(5745, node="RX02", fp="NTSC/15736/5745")])) == 2
    # The separate keys fingerprint too (compact relay lines carry video/freq_peak alone)
    fpa = mod._analog_fingerprint({"video": "NTSC", "freq_peak": 5734})
    assert fpa == {"standard": "NTSC", "freq_peak": 5734.0}, fpa
    assert mod._analog_fp_string(fpa) == "NTSC/?/5734", mod._analog_fp_string(fpa)
    assert mod._analog_fingerprint({"video": "none"}) is None
    assert mod._analog_fingerprint({"fp": "NTSC/NaN/Infinity"}) == {"standard": "NTSC"}
    assert mod._analog_fp_compatible(fpa, None) and mod._analog_fp_compatible(None, None)
    assert not mod._analog_fp_compatible(fpa, {"standard": "PAL"})
    print("  band plans: Raceband/A/B/F each keep every channel; fingerprints split same-channel  OK")


def test_bearing_input_guards(mod):
    """Over-the-air values the solver must never choke on: a null σ keeps the
    bearing at the default σ, a NaN bearing is dropped, σ is floored."""
    setup(mod, ["RX01"])
    mac = mac_for(5800, "F", 4)
    base = {"type": "analog_fm", "mac": mac, "freq_mhz": 5800, "band": "F", "ch": 4,
            "rssi_raw": 900, "rssi_dbm": -66.0, "node_id": "RX01"}
    mod.update_detection({**base, "bearing_deg": 45, "bearing_sigma_deg": None})
    o = mod.ANALOG_OBS[mac]["RX01"]
    assert o["bearing_deg"] == 45.0 and o["bearing_sigma_deg"] == mod.ANALOG_BEARING_SIGMA_DEG, o
    mod.update_detection({**base, "bearing_deg": 400, "bearing_sigma_deg": 1})
    o = mod.ANALOG_OBS[mac]["RX01"]
    assert o["bearing_deg"] == 40.0 and o["bearing_sigma_deg"] == mod.ANALOG_BEARING_SIGMA_MIN, o
    mod.update_detection({**base, "bearing_deg": float("nan"), "bearing_sigma_deg": 15})
    o = mod.ANALOG_OBS[mac]["RX01"]
    assert o["bearing_deg"] is None and o["bearing_sigma_deg"] is None, o
    # Handed straight to the solver: a zero σ is floored (no division by zero)
    # and a NaN bearing is dropped, leaving 2 observations + 1 bearing = solvable
    obs = [(0.0, 0.0, -70.0, 1.0), (500.0, 0.0, -75.0, 1.0)]
    brg = [(0.0, 0.0, 30.0, 0.0), (500.0, 0.0, 330.0, float("nan"))]
    sol = mod._analog_solve(obs, [], bearings=brg)
    assert sol is not None and sol["n_bearings"] == 1, sol
    print("  bearing guards: null σ → default, NaN → dropped, σ floored  OK")


def test_runtime_tuning_reaches_solver(mod):
    """/api/analog_fusion sets module globals; the solver's keyword defaults
    were bound at definition time, so the refresh must pass them explicitly."""
    ids = ["RX01", "RX02", "RX03", "RX04"]
    setup(mod, ids)
    seen = {}
    real_solve = mod._analog_solve

    def spy(obs, silent, **kw):
        seen.update(kw)
        return real_solve(obs, silent, **kw)

    mod._analog_solve = spy
    saved = (mod.ANALOG_PATH_LOSS_EXP, mod.ANALOG_SIGMA_DB, mod.ANALOG_SILENT_MARGIN_DB)
    try:
        mod.ANALOG_PATH_LOSS_EXP, mod.ANALOG_SIGMA_DB, mod.ANALOG_SILENT_MARGIN_DB = 2.9, 4.5, 9.0
        feed_emitter(mod, ids, 300.0, 300.0, 24.0, seed=41, channel_flip=False)
    finally:
        mod._analog_solve = real_solve
        mod.ANALOG_PATH_LOSS_EXP, mod.ANALOG_SIGMA_DB, mod.ANALOG_SILENT_MARGIN_DB = saved
    assert seen.get("n_exp") == 2.9 and seen.get("sigma_db") == 4.5 and seen.get("margin_db") == 9.0, seen
    print("  runtime tuning: n_exp/sigma_db/margin_db reach _analog_solve  OK")


def test_band_a_two_emitters(mod):
    """Two drones on adjacent Band A channels (A8 5725 / A7 5745), both heard
    by every station with legacy lines: two fixes, each on its own emitter.
    Before the width-bounded clustering this was one fix between them."""
    ids = ["RX01", "RX02", "RX03", "RX04", "RX05", "RX06"]
    setup(mod, ids)
    e1 = (350.0, 420.0)
    e2 = (-100.0, 700.0)                          # both inside the station hull
    feed_emitter(mod, ids, e1[0], e1[1], 27.0, seed=11, channel=(5725, "A", 8))
    feed_emitter(mod, ids, e2[0], e2[1], 27.0, seed=12, channel=(5745, "A", 7))
    fixes = active_fixes(mod)
    assert len(fixes) == 2, [(f["freq_mhz"], f["lat"], f["lon"]) for f in fixes]
    by_freq = {f["freq_mhz"]: f for f in fixes}
    assert set(by_freq) == {5725, 5745}, set(by_freq)
    err1 = fix_error_m(by_freq[5725], *e1)
    err2 = fix_error_m(by_freq[5745], *e2)
    print(f"  A8 @ {err1:.0f} m (±{by_freq[5725]['err_m']} m), A7 @ {err2:.0f} m (±{by_freq[5745]['err_m']} m)")
    assert err1 < 300 and err2 < 300, (err1, err2)
    # A station that hears the other emitter on a neighbouring channel is not
    # "silent" about this one: no silent penalties from busy stations
    assert by_freq[5725]["n_silent"] == 0 and by_freq[5745]["n_silent"] == 0
    assert by_freq[5725]["fix_id"] != by_freq[5745]["fix_id"]


def test_fp_same_channel_two_emitters(mod):
    """Two drones on the same channel in different parts of the mesh, one
    NTSC and one PAL. The southern stations hear one, the northern the
    other; the fingerprint keeps them apart although every MAC is shared."""
    ids = ["RX01", "RX02", "RX03", "RX04", "RX05", "RX06"]
    setup(mod, ids)
    south = ["RX01", "RX02", "RX06"]
    north = ["RX03", "RX04", "RX05"]
    e_s = (50.0, 80.0)                            # inside the southern triangle
    e_n = (300.0, 750.0)                          # inside the northern triangle
    ch = (5800, "F", 4)
    feed_emitter(mod, south, e_s[0], e_s[1], 24.0, seed=21, channel=ch,
                 extra={"fp": "NTSC/15736/5800", "video": "NTSC", "freq_peak": 5800})
    feed_emitter(mod, north, e_n[0], e_n[1], 24.0, seed=22, channel=ch,
                 extra={"fp": "PAL/15625/5800", "video": "PAL", "freq_peak": 5800})
    fixes = active_fixes(mod)
    assert len(fixes) == 2, [(f["video"], f["nodes"]) for f in fixes]
    by_std = {f["video"]: f for f in fixes}
    assert set(by_std) == {"NTSC", "PAL"}, set(by_std)
    assert {n["node_id"] for n in by_std["NTSC"]["nodes"]} == set(south)
    assert {n["node_id"] for n in by_std["PAL"]["nodes"]} == set(north)
    # Stations busy with the other emitter on this channel add no silent penalty
    assert by_std["NTSC"]["n_silent"] == 0 and by_std["PAL"]["n_silent"] == 0
    err_s = fix_error_m(by_std["NTSC"], *e_s)
    err_n = fix_error_m(by_std["PAL"], *e_n)
    print(f"  same channel: NTSC fix {err_s:.0f} m from south emitter (±{by_std['NTSC']['err_m']} m, "
          f"{by_std['NTSC']['quality']}), PAL fix {err_n:.0f} m from north "
          f"(±{by_std['PAL']['err_m']} m, {by_std['PAL']['quality']})")
    assert err_s < fix_error_m(by_std["NTSC"], *e_n)
    assert err_n < fix_error_m(by_std["PAL"], *e_s)
    # Three stations and no silent ones is an exactly-determined RSSI problem,
    # so the point can wander with the noise; the claimed radius must cover it
    assert err_s <= 2 * by_std["NTSC"]["err_m"], (err_s, by_std["NTSC"]["err_m"])
    assert err_n <= 2 * by_std["PAL"]["err_m"], (err_n, by_std["PAL"]["err_m"])
    assert by_std["NTSC"]["fp"] == "NTSC/15736/5800"
    # Fix ids survive a second round in the same assignment: the shared MACs
    # must not swap the two fixes' identities
    ids_before = {by_std["NTSC"]["fix_id"], by_std["PAL"]["fix_id"]}
    feed_emitter(mod, south, e_s[0], e_s[1], 24.0, seed=23, channel=ch,
                 extra={"fp": "NTSC/15736/5800"})
    feed_emitter(mod, north, e_n[0], e_n[1], 24.0, seed=24, channel=ch,
                 extra={"fp": "PAL/15625/5800"})
    after = {f["video"]: f for f in active_fixes(mod)}
    assert after["NTSC"]["fix_id"] == by_std["NTSC"]["fix_id"], (after["NTSC"]["fix_id"], ids_before)
    assert after["PAL"]["fix_id"] == by_std["PAL"]["fix_id"]
    # A later report without video keeps the station's last fingerprint
    feed_emitter(mod, ["RX01"], e_s[0], e_s[1], 24.0, seed=25, channel=ch)
    assert mod.ANALOG_OBS[mac_for(*ch)]["RX01"]["fp"]["standard"] == "NTSC"


def test_bearing_two_stations(mod):
    """Two stations, nobody else alive, so differential RSSI alone is
    underdetermined (no fix). With a v2 bearing from each, the two rays
    intersect and the fix is bounded, with no transmitter-power assumption.

    At the four-sector DF's σ = 15° the 90 % wedge is ±32° wide, so the
    bearings must differ by more than 64° for the region to close: the
    target sits between the stations, not far off the baseline."""
    ids = ["RX01", "RX02"]
    setup(mod, ids)
    ex, ey = 250.0, 250.0
    feed_emitter(mod, ids, ex, ey, 24.0, seed=31, channel_flip=False)
    assert not active_fixes(mod), active_fixes(mod)
    setup(mod, ids)
    heard = feed_emitter(mod, ids, ex, ey, 24.0, seed=31, channel_flip=False,
                         bearing_noise_deg=6.0, bearing_sigma_deg=15)
    assert heard == ids
    fix = best_fix(mod)
    err = fix_error_m(fix, ex, ey)
    print(f"  2 stations + 2 bearings: err={err:.0f} m (±{fix['err_m']} m, {fix['quality']}, "
          f"bounded={fix['bounded']}, bearing rms {fix['bearing_rms_deg']}°)")
    assert fix["n_bearings"] == 2
    assert fix["bounded"], fix
    assert err < 300, err
    assert fix["quality"] in ("good", "fair")
    assert all(n["bearing_deg"] is not None for n in fix["nodes"])


def test_bearing_tightens_rssi_fix(mod):
    """Same six-station RSSI scenario as test_fix_four_stations with bearings
    added: the fix must not get worse, and the reported region must shrink."""
    ids = ["RX01", "RX02", "RX03", "RX04", "RX05", "RX06"]
    ex, ey, tx = 350.0, 420.0, 27.0
    setup(mod, ids)
    feed_emitter(mod, ids, ex, ey, tx)
    base = best_fix(mod)
    base_err, base_region = fix_error_m(base, ex, ey), base["err_m"]
    setup(mod, ids)
    feed_emitter(mod, ids, ex, ey, tx, bearing_noise_deg=6.0, bearing_sigma_deg=15)
    fix = best_fix(mod)
    err = fix_error_m(fix, ex, ey)
    print(f"  RSSI only: {base_err:.0f} m (±{base_region} m); with bearings: {err:.0f} m (±{fix['err_m']} m)")
    assert fix["n_bearings"] == 6
    assert fix["err_m"] <= base_region, (fix["err_m"], base_region)
    assert err < 250, err


def main():
    mod = load_mapper()
    tests = [test_solver_direct, test_legacy_ring, test_compact_line_backfill,
             test_single_station_no_fix, test_fix_four_stations,
             test_unknown_tx_power, test_two_positive_plus_silent,
             test_cluster_band_plans, test_band_a_two_emitters,
             test_fp_same_channel_two_emitters, test_bearing_two_stations,
             test_bearing_tightens_rssi_fix, test_bearing_input_guards,
             test_runtime_tuning_reaches_solver]
    failed = 0
    for t in tests:
        print(f"[{t.__name__}]")
        try:
            t(mod)
        except AssertionError as exc:
            failed += 1
            tb = traceback.extract_tb(exc.__traceback__)[-1]
            print(f"  FAIL line {tb.lineno}: {tb.line}\n        {exc}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
