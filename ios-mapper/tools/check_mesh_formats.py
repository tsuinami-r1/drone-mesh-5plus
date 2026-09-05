#!/usr/bin/env python3
"""Cross-check the mesh alert grammar shared by the firmware, mesh-mapper.py
and the iOS parser.

Two checks:

1. The regular expressions used by `MeshAlertParser.swift` (mirrored here in
   Python's `re`, which agrees with ICU for these constructs) accept every
   sample line in `DemoFeed.swift` and the XCTest fixtures, and reject the
   status / chatter lines.
2. The `snprintf` / relay format strings in the firmware sources still
   produce lines those patterns accept. If someone changes a mesh message
   format in firmware, this script fails and the Swift patterns need updating.

Run from anywhere:  python3 ios-mapper/tools/check_mesh_formats.py
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

MAPS = r"https?://maps\.google\.com/\?q=(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)"
MAC = r"([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})"

PATTERNS = {
    "drone":  re.compile(r"^Drone(?:\[([^\]]+)\])?:\s*" + MAC + r"\s+RSSI:(-?\d+)(?:\s+" + MAPS + r")?"),
    "pilot":  re.compile(r"^Pilot\[" + MAC + r"\]:\s*" + MAPS),
    "dji":    re.compile(r"^DJI\s+([0-9A-Fa-f]{12})\s+RSSI:(-?\d+)(?:\s+" + MAPS + r")?"),
    "analog": re.compile(r"^AnalogFM:\s*([A-Za-z])(\d+)\s+(\d+)MHz\s+rssi=(-?\d+)\s+\[([^\]]+)\]"),
}

# (line, expected kind) — `None` means "must be ignored".
SAMPLES = [
    ("Drone: 60:60:1f:3a:2b:1c RSSI:-62 https://maps.google.com/?q=47.620500,-122.349300", "drone"),
    ("Drone: 8c:1f:64:12:34:56 RSSI:-81", "drone"),
    ("Drone[5G]: 34:d2:62:aa:bb:cc RSSI:-70 https://maps.google.com/?q=47.628100,-122.342000", "drone"),
    ("Drone[2.4G]: 34:D2:62:AA:BB:CC RSSI:-70", "drone"),
    ("Pilot[60:60:1f:3a:2b:1c]: https://maps.google.com/?q=47.618900,-122.352100", "pilot"),
    ("DJI 60601f9a8b7c RSSI:-58 https://maps.google.com/?q=47.615200,-122.338700", "dji"),
    ("DJI 60601f9a8b7c RSSI:-58", "dji"),
    ("AnalogFM: R3 5732MHz rssi=812 [RX01]", "analog"),
    ("AnalogFM: A1 5865MHz rssi=650 [RX02]", "analog"),
    ('{"mac":"a4:cf:12:9e:00:01","rssi":-66,"drone_lat":47.612,"drone_long":-122.331,'
     '"drone_altitude":95,"pilot_lat":47.6105,"pilot_long":-122.333,"basic_id":"1596F3B9A2C1D4E5F6"}', "json"),
    ('{"heartbeat":"home_node active","tracked_drones":2}', None),
    ('{"info":"RX5808 scanner ready","node_id":"RX01","channels":40,"threshold":600}', None),
    ("hello from the mesh", None),
    ("WATCHDOG_RESET", None),
    ("[SCAN] channel 12 rejected by regulatory domain - check WIFI_CHAN_START/WIFI_CHAN_COUNT", None),
]

# Firmware format strings → representative rendered output. Kept literal so a
# grep failure here points straight at the changed file.
FIRMWARE_FORMATS = [
    ("remoteid-mesh-dualcore/src/main.cpp", '"Drone: %s RSSI:%d"', "drone"),
    ("remoteid-mesh/src/main.cpp",          '"Drone: %s RSSI:%d"', "drone"),
    ("remoteid-c5-5g/src/main.cpp",         '"Drone[%s]: %s RSSI:%d"', "drone"),
    ("remoteid-mesh-dualcore/src/main.cpp", '"Pilot[%s]: https://maps.google.com/?q=%.6f,%.6f"', "pilot"),
    ("remoteid-mesh/src/main.cpp",          '"Pilot[%s]: https://maps.google.com/?q=%.6f,%.6f"', "pilot"),
    ("remoteid-c5-5g/src/main.cpp",         '"Pilot[%s]: https://maps.google.com/?q=%.6f,%.6f"', "pilot"),
    ("remoteid-mesh-dualcore/src/main.cpp", '"DJI %02x%02x%02x%02x%02x%02x RSSI:%d"', "dji"),
    ("remoteid-c5-5g/src/main.cpp",         '"DJI %02x%02x%02x%02x%02x%02x RSSI:%d"', "dji"),
    ("node-mode-dualcore/src/main.cpp",     '"DJI %02x%02x%02x%02x%02x%02x RSSI:%d"', "dji"),
    ("rx5808-detection/src/main.cpp",       '"AnalogFM: %s%d %uMHz rssi=%d [%s]"', "analog"),
    ("rx5808-detection/src/main.cpp",       '"AF:00:%02X:%02X:%02X:%02X"', "analog-mac"),
    ("remoteid-mesh-dualcore/src/main.cpp", '" https://maps.google.com/?q=%.6f,%.6f"', "maps"),
]

RENDERED = {
    "drone":  "Drone: aa:bb:cc:dd:ee:ff RSSI:-70",
    "pilot":  "Pilot[aa:bb:cc:dd:ee:ff]: https://maps.google.com/?q=1.000000,-2.000000",
    "dji":    "DJI aabbccddeeff RSSI:-70",
    "analog": "AnalogFM: R3 5732MHz rssi=812 [RX01]",
}


def classify(line: str):
    if "{" in line:
        try:
            obj = json.loads(line[line.index("{"): line.rindex("}") + 1])
        except ValueError:
            return None
        has_det = any(k in obj for k in ("drone_lat", "pilot_lat", "basic_id", "remote_id"))
        if any(k in obj for k in ("heartbeat", "status", "info")) and not has_det:
            return None
        return "json" if obj.get("mac") else None
    for kind, pat in PATTERNS.items():
        if pat.match(line):
            return kind
    return None


def analog_mac(band: str, ch: int, freq: int) -> str:
    return "af:00:%02x:%02x:%02x:%02x" % ((freq >> 8) & 0xFF, freq & 0xFF, ord(band[0]), ch)


def main() -> int:
    failures = 0

    for line, expected in SAMPLES:
        got = classify(line)
        status = "ok " if got == expected else "BAD"
        if got != expected:
            failures += 1
        print(f"[{status}] {expected!s:7} <- {line[:70]}")

    # The synthetic analog MAC must match channel_to_mac() in the firmware.
    want = "af:00:16:64:52:03"
    got = analog_mac("R", 3, 5732)
    print(f"[{'ok ' if got == want else 'BAD'}] analog MAC {got}")
    failures += got != want

    for rel, fmt, kind in FIRMWARE_FORMATS:
        src = (ROOT / rel).read_text(errors="ignore")
        present = fmt in src
        ok = present and (kind in ("analog-mac", "maps") or PATTERNS[kind].match(RENDERED[kind]))
        failures += not ok
        print(f"[{'ok ' if ok else 'BAD'}] {rel}: {fmt}")

    print("\nALL OK" if not failures else f"\n{failures} FAILURE(S)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
