import Foundation

/// Replays representative mesh lines so the map, list and alerts can be
/// exercised in the simulator with no radio. The formats are exactly what
/// the firmware in this repository puts on the mesh.
@MainActor
final class DemoFeed {
    private weak var store: DetectionStore?
    private var timer: Timer?
    private var index = 0

    static let script: [String] = [
        // Remote ID drone with GPS, then its pilot (two-line relay)
        "Drone: 60:60:1f:3a:2b:1c RSSI:-62 https://maps.google.com/?q=47.620500,-122.349300",
        "Pilot[60:60:1f:3a:2b:1c]: https://maps.google.com/?q=47.618900,-122.352100",
        // C5 5 GHz variant with band tag
        "Drone[5G]: 34:d2:62:aa:bb:cc RSSI:-70 https://maps.google.com/?q=47.628100,-122.342000",
        // Analog FPV from node RX01 (no GPS; range ring at node position)
        "AnalogFM: R3 5732MHz rssi=812 [RX01]",
        // DJI compact relay
        "DJI 60601f9a8b7c RSSI:-58 https://maps.google.com/?q=47.615200,-122.338700",
        // Remote ID without a fix (no-GPS alert path)
        "Drone: 8c:1f:64:12:34:56 RSSI:-81",
        // Movement for the first drone
        "Drone: 60:60:1f:3a:2b:1c RSSI:-60 https://maps.google.com/?q=47.621400,-122.347900",
        "Drone: 60:60:1f:3a:2b:1c RSSI:-59 https://maps.google.com/?q=47.622300,-122.346500",
        // node-mode-dualcore relays the full JSON object instead of text
        #"{"mac":"a4:cf:12:9e:00:01","rssi":-66,"drone_lat":47.612000,"drone_long":-122.331000,"drone_altitude":95,"pilot_lat":47.610500,"pilot_long":-122.333000,"basic_id":"1596F3B9A2C1D4E5F6"}"#,
        // Heartbeat frames must be ignored
        #"{"heartbeat":"home_node active","tracked_drones":2}"#,
        "AnalogFM: R3 5732MHz rssi=790 [RX01]",
        "AnalogFM: A1 5865MHz rssi=650 [RX02]",
    ]

    init(store: DetectionStore) {
        self.store = store
    }

    /// Stand-in node positions for the `[RX01]` / `[RX02]` analog alerts in
    /// the script, since no Meshtastic node DB exists in demo mode.
    static func nodePosition(forNodeID nodeID: String?) -> (lat: Double, lon: Double)? {
        switch nodeID?.uppercased() {
        case "RX01": return (47.6240, -122.3400)
        case "RX02": return (47.6105, -122.3560)
        default:     return nil
        }
    }

    func start() {
        stop()
        index = 0
        timer = Timer.scheduledTimer(withTimeInterval: 2.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
        tick()
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private func tick() {
        guard let store else { return }
        let line = Self.script[index % Self.script.count]
        index += 1
        store.ingest(line: line, sender: 0xDEAD_BEEF)
    }
}
