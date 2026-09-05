import Foundation

/// Parses the text that detection nodes push over the Meshtastic mesh into
/// `DetectionUpdate` patches.
///
/// Two families of payload travel over the mesh, matching the firmware in this
/// repository:
///
/// 1. Human-readable relays (remoteid-mesh, remoteid-mesh-dualcore,
///    remoteid-c5-5g, rx5808-detection):
///
///        Drone: aa:bb:cc:dd:ee:ff RSSI:-71 https://maps.google.com/?q=48.123456,-122.123456
///        Drone[5G]: aa:bb:cc:dd:ee:ff RSSI:-71 https://maps.google.com/?q=...
///        Pilot[aa:bb:cc:dd:ee:ff]: https://maps.google.com/?q=48.1,-122.1
///        DJI aabbccddeeff RSSI:-60 https://maps.google.com/?q=...
///        AnalogFM: R3 5732MHz rssi=712 [RX01]
///
/// 2. The full JSON detection object (node-mode-dualcore remote nodes), which
///    is the same schema `serial_reader()` in mesh-mapper.py accepts:
///
///        {"mac":"..","rssi":-71,"drone_lat":..,"drone_long":..,"drone_altitude":..,
///         "pilot_lat":..,"pilot_long":..,"basic_id":".."}
///
/// A single mesh message may carry several newline-separated lines, so
/// `parse(message:)` returns every update it finds.
enum MeshAlertParser {

    // MARK: - Public API

    /// Parse one mesh text message (possibly multi-line) into updates.
    static func parse(message: String) -> [DetectionUpdate] {
        var results: [DetectionUpdate] = []
        for rawLine in message.split(whereSeparator: { $0 == "\n" || $0 == "\r" }) {
            let line = rawLine.trimmingCharacters(in: .whitespacesAndNewlines)
            if line.isEmpty { continue }
            if let update = parse(line: line) {
                results.append(update)
            }
        }
        return results
    }

    /// Parse one line. Returns nil for status/heartbeat frames and unrelated chatter.
    static func parse(line: String) -> DetectionUpdate? {
        if line.contains("{") {
            return parseJSON(line: line)
        }
        if let m = droneRegex.firstMatch(in: line) {
            var u = DetectionUpdate(mac: normalizeMAC(m[2]!), kind: .remoteID)
            u.band = m[1]
            u.rssi = Int(m[3]!)
            if let lat = m[4].flatMap({ Double($0) }), let lon = m[5].flatMap({ Double($0) }) {
                u.droneLat = lat
                u.droneLon = lon
            }
            return u
        }
        if let m = pilotRegex.firstMatch(in: line) {
            var u = DetectionUpdate(mac: normalizeMAC(m[1]!), kind: .remoteID)
            u.isPilotOnly = true
            u.pilotLat = Double(m[2]!)
            u.pilotLon = Double(m[3]!)
            return u
        }
        if let m = djiRegex.firstMatch(in: line) {
            var u = DetectionUpdate(mac: normalizeMAC(m[1]!), kind: .dji)
            u.rssi = Int(m[2]!)
            if let lat = m[3].flatMap({ Double($0) }), let lon = m[4].flatMap({ Double($0) }) {
                u.droneLat = lat
                u.droneLon = lon
            }
            return u
        }
        if let m = analogRegex.firstMatch(in: line) {
            let band = m[1]!.uppercased()
            let ch = Int(m[2]!)!
            let freq = Int(m[3]!)!
            var u = DetectionUpdate(mac: analogFMMAC(band: band, channel: ch, freqMHz: freq),
                                    kind: .analogFM)
            u.band = band
            u.channel = ch
            u.freqMHz = freq
            u.rssiRaw = Int(m[4]!)
            u.rssi = u.rssiRaw
            u.nodeID = m[5]!
            u.basicID = "5.8G-\(band)\(ch)-\(freq)MHz"
            return u
        }
        return nil
    }

    // MARK: - JSON payloads

    static func parseJSON(line: String) -> DetectionUpdate? {
        // Same extraction as serial_reader(): from the first '{' to the last '}'.
        guard let start = line.firstIndex(of: "{") else { return nil }
        let end = line.lastIndex(of: "}").map { line.index(after: $0) } ?? line.endIndex
        let jsonText = end > start ? String(line[start..<end]) : String(line[start...])
        guard let data = jsonText.data(using: .utf8),
              let obj = try? JSONSerialization.jsonObject(with: data),
              let dict = obj as? [String: Any] else { return nil }
        return parse(json: dict)
    }

    static func parse(json d: [String: Any]) -> DetectionUpdate? {
        let detectionKeys = ["drone_lat", "pilot_lat", "basic_id", "remote_id"]
        let hasDetectionKey = detectionKeys.contains { d[$0] != nil }

        // Status/info/heartbeat frames carry no detection payload — skip them
        // before anything else, exactly as serial_reader() does.
        if (d["heartbeat"] != nil || d["status"] != nil || d["info"] != nil) && !hasDetectionKey {
            return nil
        }
        guard let rawMAC = d["mac"] as? String, !rawMAC.isEmpty else { return nil }

        let typeField = (d["type"] as? String)?.lowercased()
        let idType = (d["id_type"] as? String)?.uppercased()
        let kind: DetectionKind
        if typeField == "analog_fm" {
            kind = .analogFM
        } else if idType == "DJI" {
            kind = .dji
        } else {
            kind = .remoteID
        }

        var u = DetectionUpdate(mac: normalizeMAC(rawMAC), kind: kind)
        u.rssi = intValue(d["rssi"])
        u.band = d["band"] as? String
        u.droneLat = doubleValue(d["drone_lat"])
        u.droneLon = doubleValue(d["drone_long"] ?? d["drone_lon"])
        u.droneAltitude = doubleValue(d["drone_altitude"])
        u.pilotLat = doubleValue(d["pilot_lat"])
        u.pilotLon = doubleValue(d["pilot_long"] ?? d["pilot_lon"])
        // Normalize remote_id → basic_id, as serial_reader() does.
        u.basicID = (d["basic_id"] as? String) ?? (d["remote_id"] as? String)
        u.nodeID = d["node_id"] as? String
        if kind == .analogFM {
            u.freqMHz = intValue(d["freq_mhz"])
            u.channel = intValue(d["ch"] ?? d["channel"])
            u.rssiRaw = intValue(d["rssi_raw"]) ?? u.rssi
        } else {
            u.channel = intValue(d["channel"])
        }
        return u
    }

    // MARK: - Helpers

    /// Lower-case `aa:bb:cc:dd:ee:ff`. Accepts colon-separated or bare 12-hex form.
    static func normalizeMAC(_ raw: String) -> String {
        let hex = raw.filter { $0.isHexDigit }.lowercased()
        guard hex.count == 12 else { return raw.lowercased() }
        var parts: [String] = []
        var idx = hex.startIndex
        while idx < hex.endIndex {
            let next = hex.index(idx, offsetBy: 2)
            parts.append(String(hex[idx..<next]))
            idx = next
        }
        return parts.joined(separator: ":")
    }

    /// Same synthetic MAC the RX5808 firmware emits in its JSON
    /// (`channel_to_mac()` in rx5808-detection/src/main.cpp), so the text relay
    /// and the JSON form of one FPV channel merge into a single tracked entry.
    static func analogFMMAC(band: String, channel: Int, freqMHz: Int) -> String {
        let bandByte = band.uppercased().utf8.first ?? 0
        return String(format: "af:00:%02x:%02x:%02x:%02x",
                      (freqMHz >> 8) & 0xFF, freqMHz & 0xFF, Int(bandByte), channel & 0xFF)
    }

    private static func intValue(_ v: Any?) -> Int? {
        switch v {
        case let n as Int: return n
        case let n as Double: return Int(n)
        case let n as NSNumber: return n.intValue
        case let s as String: return Int(s)
        default: return nil
        }
    }

    private static func doubleValue(_ v: Any?) -> Double? {
        switch v {
        case let n as Double: return n
        case let n as Int: return Double(n)
        case let n as NSNumber: return n.doubleValue
        case let s as String: return Double(s)
        default: return nil
        }
    }

    // MARK: - Patterns (kept in sync with the firmware snprintf formats)

    private static let mapsURL = #"https?://maps\.google\.com/\?q=(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?)"#

    /// `Drone: <mac> RSSI:<n> [url]` and `Drone[<band>]: <mac> RSSI:<n> [url]`
    static let droneRegex = LineRegex(
        #"^Drone(?:\[([^\]]+)\])?:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\s+RSSI:(-?\d+)(?:\s+"# + mapsURL + #")?"#)

    /// `Pilot[<mac>]: <url>`
    static let pilotRegex = LineRegex(
        #"^Pilot\[([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\]:\s*"# + mapsURL)

    /// `DJI <12 hex> RSSI:<n> [url]`
    static let djiRegex = LineRegex(
        #"^DJI\s+([0-9A-Fa-f]{12})\s+RSSI:(-?\d+)(?:\s+"# + mapsURL + #")?"#)

    /// `AnalogFM: <band><ch> <freq>MHz rssi=<n> [<node>]`
    static let analogRegex = LineRegex(
        #"^AnalogFM:\s*([A-Za-z])(\d+)\s+(\d+)MHz\s+rssi=(-?\d+)\s+\[([^\]]+)\]"#)
}

/// Thin wrapper over NSRegularExpression that returns capture groups as optionals.
struct LineRegex {
    let regex: NSRegularExpression

    init(_ pattern: String) {
        // Patterns are compile-time constants; a failure here is a programming error.
        regex = try! NSRegularExpression(pattern: pattern, options: [])
    }

    struct Match {
        let groups: [String?]
        subscript(_ i: Int) -> String? { i < groups.count ? groups[i] : nil }
    }

    func firstMatch(in text: String) -> Match? {
        let ns = text as NSString
        guard let m = regex.firstMatch(in: text, options: [], range: NSRange(location: 0, length: ns.length)) else {
            return nil
        }
        var groups: [String?] = []
        for i in 0..<m.numberOfRanges {
            let r = m.range(at: i)
            groups.append(r.location == NSNotFound ? nil : ns.substring(with: r))
        }
        return Match(groups: groups)
    }
}
