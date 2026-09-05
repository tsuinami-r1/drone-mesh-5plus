# Mesh Mapper for iOS (prototype)

An iPhone counterpart to `mesh-mapper.py`. Instead of a laptop with a "home"
node on USB, the phone connects **directly to a Meshtastic radio over
Bluetooth**, listens to the text alerts the detection nodes push over the LoRa
mesh, and turns them into a live map, a detection list and lock-screen
notifications — including while the app is in the background.

```
 detection node ──UART──▶ Heltec (Meshtastic) ──LoRa──▶ ... ──LoRa──▶ your Meshtastic radio
                                                                          │ BLE
                                                                          ▼
                                                              Mesh Mapper (this app)
```

## What it does

| Desktop mapper | iOS prototype |
|---|---|
| `serial_reader()` reads JSON from the home node on USB | `MeshtasticRadio` reads `FromRadio` protobufs over BLE and pulls out text packets |
| Parses the firmware JSON detection object | `MeshAlertParser` parses **both** the human-readable mesh relays (`Drone:`, `Drone[5G]:`, `Pilot[mac]:`, `DJI …`, `AnalogFM: …`) and the JSON object that node-mode firmware relays |
| `update_detection()` / `tracked_pairs` | `DetectionStore.apply()` — same MAC keying, 30 s GPS carry-forward, `basic_id` preservation, analog-FM exclusion from no-GPS logic |
| `cleanup_old_detections()` | `DetectionStore.sweep()` — `inactive` at 3× stale threshold, `inactive_old` at 30×, analog rings dropped after 30 s silence |
| `_fetch_meshtastic_position` (HTTP poll of the Meshtastic API) | Node DB comes straight from the connected radio (`NodeInfo` + live `POSITION_APP` packets); `[RX01]` in an analog alert is resolved by short/long name, falling back to the relaying node |
| `_rssi_to_max_range_m` | `RangeEstimator` — identical free-space-path-loss formula for 5.8 GHz range rings |
| Webhook / popup on activation | `UNUserNotificationCenter` time-sensitive local notifications |
| Leaflet map | MapKit: drone + pilot markers, drone→pilot link, flight track polyline, 📡 range rings at node position, known mesh nodes |
| `aliases.json`, `detections_*.csv` | Aliases in UserDefaults; CSV export with the same column header via the share sheet |

Not carried over (yet): FAA registry lookup, TAK/CoT multicast, KML, webhooks,
peer-server sync, the web UI's colour/lock controls.

## Running in the background

The point of the app is to keep listening while the phone is in a pocket.
Three mechanisms cooperate:

1. **`bluetooth-central` background mode** (Info.plist `UIBackgroundModes`).
   While the BLE link is up, every `fromNum` notification from the radio wakes
   the app for a few seconds, it drains `fromRadio`, updates the store and
   posts notifications, then is suspended again. There is no polling and no
   timer; the radio does the waking.
2. **CoreBluetooth state restoration** (`CBCentralManagerOptionRestoreIdentifierKey`
   in `MeshtasticRadio`). If iOS terminates the suspended app for memory, the
   next BLE event relaunches it in the background and `willRestoreState` hands
   the peripheral back. This is why `AppModel` (and with it the central
   manager) is created by the app delegate at launch and not lazily by a view.
3. **Persistent reconnect.** A pending `connect()` never times out on iOS and
   survives backgrounding, so when the radio goes out of range and returns, or
   reboots, the link comes back without user action. The saved peripheral UUID
   is retried on every Bluetooth power-on too.

Session state (`detections`, history, alert log) is written to Application
Support on every sweep and when the app backgrounds, so data collected while
you were not looking survives a relaunch.

What background mode does **not** give you: the app is not running
continuously; it is woken per packet. That is exactly what we want — the
mesh is bursty and the phone battery is finite. Notifications use the
`time-sensitive` interruption level (entitlement in `project.yml`) so they
break through Focus modes.

## Building

The Xcode project is generated from `project.yml` with
[XcodeGen](https://github.com/yonaskolb/XcodeGen); the `.xcodeproj`,
`Info.plist` and entitlements are derived files and are git-ignored.

```bash
brew install xcodegen
cd ios-mapper
xcodegen generate
open MeshMapper.xcodeproj
```

Then set your team under *Signing & Capabilities* (or uncomment
`DEVELOPMENT_TEAM` in `project.yml`) and run on a device. Bluetooth does not
exist in the simulator, so use the **Demo feed** toggle in *Settings* there;
it replays the sample lines in `DemoFeed.swift` — every mesh format the
firmware in this repo produces — every 2.5 s.

Requirements: iOS 17 (SwiftUI `Map` with `MapCircle` / `MapPolyline`),
Xcode 15+. No third-party dependencies; the protobuf decoding is a small
hand-rolled reader (`ProtobufReader.swift`) covering only the Meshtastic
fields the app needs.

Unit tests (`⌘U`) cover the parser against every format, the protobuf
reader/writer with hand-assembled `FromRadio` frames, and the store's
carry-forward, ageing, alert and CSV behaviour.

## Pairing a radio

1. Pair the Heltec/T-Beam/whatever with the phone once in the official
   Meshtastic app if it asks for a PIN — the bond is system-wide.
2. **Disconnect it in the Meshtastic app.** A Meshtastic radio accepts a
   single BLE client; the two apps cannot share one radio. (Two radios, one
   per app, works fine.)
3. In Mesh Mapper → *Radio* → *Scan for radios* → tap it. The app writes
   `want_config_id`, downloads the node DB, and shows *Connected*. From then on
   it reconnects by itself.

The radio must be on the same channel (and hold the same PSK) as the detection
nodes' radios; packets on other channels arrive encrypted and are ignored.

### Naming nodes for range rings

Exactly like the desktop mapper: the Meshtastic node paired with an RX5808
node should have `shortName` or `longName` equal to the firmware `NODE_ID`
(e.g. `RX01`). The app first matches the name, then falls back to the position
of the node that relayed the packet — which is usually the right radio anyway,
since the detection node's own Heltec is the sender.

## Mesh message formats handled

```
Drone: aa:bb:cc:dd:ee:ff RSSI:-71 https://maps.google.com/?q=47.620500,-122.349300
Drone[5G]: aa:bb:cc:dd:ee:ff RSSI:-71 https://maps.google.com/?q=…       (remoteid-c5-5g)
Pilot[aa:bb:cc:dd:ee:ff]: https://maps.google.com/?q=47.618900,-122.352100
DJI aabbccddeeff RSSI:-60 https://maps.google.com/?q=…
AnalogFM: R3 5732MHz rssi=812 [RX01]                                      (rx5808-detection)
{"mac":"…","rssi":-66,"drone_lat":…,"drone_long":…,"drone_altitude":…,
 "pilot_lat":…,"pilot_long":…,"basic_id":"…"}                             (node-mode-dualcore)
```

Heartbeat / status / info JSON frames and any other chatter are ignored, with
the same rule as `serial_reader()`. The DJI bare MAC is normalised to the
colon form so it merges with the JSON form from the same node, and analog FM
uses the firmware's synthetic `AF:00:…` MAC (`channel_to_mac()`), so both
forms of one FPV channel land on one entry.

`tools/check_mesh_formats.py` re-derives these patterns in Python, runs them
over the sample lines, and greps the firmware for the `snprintf` format
strings — run it after touching any mesh message format:

```bash
python3 ios-mapper/tools/check_mesh_formats.py
```

## Layout

```
ios-mapper/
├── project.yml                     XcodeGen spec (background mode, usage strings, entitlement)
├── MeshMapper/
│   ├── App/MeshMapperApp.swift     AppModel wiring, app delegate, notification delegate
│   ├── Models/Detection.swift      Detection / DetectionUpdate / MeshNode
│   ├── Parsing/MeshAlertParser.swift
│   ├── Meshtastic/
│   │   ├── MeshtasticRadio.swift   CoreBluetooth client, node DB, restoration, reconnect
│   │   ├── MeshtasticMessages.swift FromRadio / MeshPacket / NodeInfo / Position decoders
│   │   └── ProtobufReader.swift    varint / fixed32 / length-delimited wire reader + writer
│   ├── Store/
│   │   ├── DetectionStore.swift    tracked_pairs + update_detection + cleanup + alerts + CSV
│   │   ├── AlertNotifier.swift     local notifications
│   │   ├── RangeEstimator.swift    5.8 GHz FSPL range
│   │   ├── LocationProvider.swift  phone position for distance readouts
│   │   └── MapperSettings.swift
│   ├── Views/                      Map, Detections (+detail, alias editor), Alerts, Radio, Settings
│   └── Demo/DemoFeed.swift         simulator replay of every mesh format
├── MeshMapperTests/                parser, protobuf, store tests
└── tools/check_mesh_formats.py     firmware ⇄ parser grammar check
```

## Known limitations of the prototype

- Not compiled in CI here: the repo has no macOS runner. Build with Xcode.
- Uses the current `fromRadio` UUID (`2c55e69e-…`); radios on very old
  firmware that only expose the legacy `8ba2bcc2-…` characteristic will show
  "missing a required characteristic".
- Only text (`TEXT_MESSAGE_APP`), position and node-info ports are decoded.
  Detection nodes attached through the Meshtastic serial module in
  `TEXTMSG` mode broadcast as text, which is what all firmware here uses.
- If the user has the Meshtastic app connected to the same radio, this app
  will sit in "Connecting…" until that app lets go.
