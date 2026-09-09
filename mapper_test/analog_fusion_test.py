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

HERE = os.path.dirname(os.path.abspath(__file__))
SRC  = os.path.join(os.path.dirname(HERE), "mesh-mapper.py")


def load_mapper():
    tmp = tempfile.mkdtemp(prefix="mapper_test_")
    dst = os.path.join(tmp, "mesh_mapper_under_test.py")
    shutil.copy(SRC, dst)
    spec = importlib.util.spec_from_file_location("mesh_mapper_under_test", dst)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.TAK_ENABLE = False
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
                 channel_flip=True):
    """Feed one report per station that can hear the emitter. Returns the
    station ids that reported."""
    rng = random.Random(seed)
    heard = []
    for i, nid in enumerate(station_ids):
        sx, sy = STATIONS[nid]
        dbm = simulate_rx_dbm(tx_dbm, ex, ey, sx, sy, n_exp, noise_db, rng)
        if dbm < RX_THRESHOLD_DBM:
            continue
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
        mod.update_detection(det)
        heard.append(nid)
    return heard


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


def main():
    mod = load_mapper()
    tests = [test_solver_direct, test_legacy_ring, test_compact_line_backfill,
             test_single_station_no_fix, test_fix_four_stations,
             test_unknown_tx_power, test_two_positive_plus_silent]
    failed = 0
    for t in tests:
        print(f"[{t.__name__}]")
        try:
            t(mod)
        except AssertionError as exc:
            failed += 1
            print(f"  FAIL: {exc}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
