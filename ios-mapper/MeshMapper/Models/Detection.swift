import Foundation
import CoreLocation

/// What kind of emitter a detection came from.
///
/// Mirrors the `type` / `id_type` fields that `mesh-mapper.py` keys its
/// OpenDroneID-only code paths on. `analogFM` must never bleed into the
/// Remote ID logic (popups, FAA lookup, no-GPS alerts) — see `isNoGPSDrone`.
enum DetectionKind: String, Codable, CaseIterable {
    /// ASTM F3411 / ASD-STAN Remote ID, or MAVLink GPS relayed as Remote ID.
    case remoteID = "remote_id"
    /// DJI DroneID (Wi-Fi IE 221, OUI 26:37:12).
    case dji = "dji"
    /// RX5808 analog 5.8 GHz FPV video carrier. No drone or pilot GPS by design.
    case analogFM = "analog_fm"

    var label: String {
        switch self {
        case .remoteID: return "Remote ID"
        case .dji:      return "DJI DroneID"
        case .analogFM: return "Analog FPV"
        }
    }

    var symbol: String {
        switch self {
        case .remoteID: return "airplane"
        case .dji:      return "airplane.circle"
        case .analogFM: return "antenna.radiowaves.left.and.right"
        }
    }
}

/// Same three states `cleanup_old_detections()` assigns in mesh-mapper.py.
enum DetectionStatus: String, Codable {
    case active
    case inactive
    case inactiveOld = "inactive_old"
}

struct TrackPoint: Codable, Equatable {
    let lat: Double
    let lon: Double
    let time: Date
}

/// One tracked emitter, keyed by MAC. This is the iOS equivalent of an entry
/// in `tracked_pairs` in mesh-mapper.py.
struct Detection: Identifiable, Codable, Equatable {
    var id: String { mac }

    /// Lower-cased `aa:bb:cc:dd:ee:ff`. Analog FM uses the RX5808 firmware's
    /// synthetic `AF:00:..` MAC (see `MeshAlertParser.analogFMMAC`).
    var mac: String
    var kind: DetectionKind

    var rssi: Int?
    /// `"2.4G"` / `"5G"` from the C5 firmware, or the FPV band letter (A/B/E/F/R/L) for analog.
    var band: String?

    var droneLat: Double = 0
    var droneLon: Double = 0
    var droneAltitude: Double = 0
    var pilotLat: Double = 0
    var pilotLon: Double = 0

    var basicID: String?
    /// Detection node name (`NODE_ID` in firmware, e.g. "RX01"). Present for analog FM.
    var nodeID: String?
    /// Meshtastic node number of the radio that relayed the alert.
    var senderNodeNum: UInt32?
    /// Human-readable name of the relaying Meshtastic node, if known.
    var senderName: String?

    // Analog FM range ring (node position + estimated max range).
    var nodeLat: Double?
    var nodeLon: Double?
    var radiusM: Double?
    var freqMHz: Int?
    var channel: Int?
    var rssiRaw: Int?

    var firstSeen: Date
    var lastUpdate: Date
    /// Time of the last advert that actually carried a GPS fix (not a carried-forward one).
    var lastGPSUpdate: Date?
    var status: DetectionStatus = .active
    var track: [TrackPoint] = []

    var hasDroneGPS: Bool { droneLat != 0 && droneLon != 0 }
    var hasPilotGPS: Bool { pilotLat != 0 && pilotLon != 0 }

    /// Mirrors `isNoGpsDrone` in the mapper JS: a drone with neither fix.
    /// Always guarded against analog FM, which has no GPS by design.
    var isNoGPSDrone: Bool { !hasDroneGPS && !hasPilotGPS && kind != .analogFM }

    var droneCoordinate: CLLocationCoordinate2D? {
        hasDroneGPS ? CLLocationCoordinate2D(latitude: droneLat, longitude: droneLon) : nil
    }
    var pilotCoordinate: CLLocationCoordinate2D? {
        hasPilotGPS ? CLLocationCoordinate2D(latitude: pilotLat, longitude: pilotLon) : nil
    }
    var nodeCoordinate: CLLocationCoordinate2D? {
        guard let lat = nodeLat, let lon = nodeLon, lat != 0, lon != 0 else { return nil }
        return CLLocationCoordinate2D(latitude: lat, longitude: lon)
    }

    func displayName(aliases: [String: String]) -> String {
        if let alias = aliases[mac], !alias.isEmpty { return alias }
        if kind == .analogFM, let band, let channel, let freqMHz {
            return "\(band)\(channel) \(freqMHz) MHz"
        }
        if let basicID, !basicID.isEmpty { return basicID }
        return mac
    }
}

/// A normalized patch produced by `MeshAlertParser` from one mesh line.
/// `nil` means "field not present in this message" so the store can merge
/// it against the existing tracked entry (carry-forward semantics).
struct DetectionUpdate: Equatable {
    var mac: String
    var kind: DetectionKind
    var rssi: Int?
    var band: String?
    var droneLat: Double?
    var droneLon: Double?
    var droneAltitude: Double?
    var pilotLat: Double?
    var pilotLon: Double?
    var basicID: String?
    var nodeID: String?
    var freqMHz: Int?
    var channel: Int?
    var rssiRaw: Int?
    /// `Pilot[mac]:` lines carry only the operator position. They must not
    /// clear the drone position or RSSI that the preceding `Drone:` line set.
    var isPilotOnly: Bool = false

    init(mac: String, kind: DetectionKind) {
        self.mac = mac
        self.kind = kind
    }
}

/// A Meshtastic node the connected radio knows about (its node DB), used to
/// resolve `[RX01]` in analog FM alerts to a position — the same job as
/// `_fetch_meshtastic_position` in mesh-mapper.py, but from the live BLE link
/// instead of the HTTP API.
struct MeshNode: Identifiable, Equatable {
    var id: UInt32 { num }
    var num: UInt32
    var shortName: String = ""
    var longName: String = ""
    var lat: Double?
    var lon: Double?
    var altitude: Double?
    var lastHeard: Date?

    var coordinate: CLLocationCoordinate2D? {
        guard let lat, let lon, lat != 0, lon != 0 else { return nil }
        return CLLocationCoordinate2D(latitude: lat, longitude: lon)
    }

    var displayName: String {
        if !longName.isEmpty { return longName }
        if !shortName.isEmpty { return shortName }
        return String(format: "!%08x", num)
    }
}
