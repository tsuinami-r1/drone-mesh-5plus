import Foundation
import Combine

/// One row of the session log — the same columns `mesh-mapper.py` writes to
/// `detections_<session>.csv`.
struct HistoryRow: Codable {
    let timestamp: Date
    let alias: String
    let mac: String
    let rssi: Int?
    let droneLat: Double
    let droneLon: Double
    let droneAltitude: Double
    let pilotLat: Double
    let pilotLon: Double
    let basicID: String
}

/// A user-visible event (what would have been a webhook / popup on the desktop mapper).
struct AlertEvent: Identifiable, Codable {
    var id = UUID()
    let time: Date
    let mac: String
    let title: String
    let body: String
}

/// In-memory model of everything the mesh has reported, mirroring
/// `tracked_pairs` / `update_detection()` / `cleanup_old_detections()` in
/// mesh-mapper.py. All access is on the main actor.
@MainActor
final class DetectionStore: ObservableObject {

    @Published private(set) var detections: [String: Detection] = [:]
    @Published private(set) var history: [HistoryRow] = []
    @Published private(set) var events: [AlertEvent] = []
    @Published var aliases: [String: String] = [:] {
        didSet { persistAliases() }
    }
    @Published var settings = MapperSettings.load() {
        didSet { settings.save() }
    }

    /// Resolves a detection node's position for analog FM range rings.
    /// Wired to `MeshtasticRadio.position(forNodeID:senderNum:)` by the app.
    var nodePositionResolver: (@MainActor (String?, UInt32?) -> (lat: Double, lon: Double)?)?
    /// Human-readable name of the Meshtastic node that relayed a packet.
    var senderNameResolver: (@MainActor (UInt32) -> String?)?

    let notifier: AlertNotifier

    // Alert bookkeeping (backend_previous_active / backend_alerted_no_gps / backend_seen_drones).
    private var previousActive: [String: Bool] = [:]
    private var alertedNoGPS: Set<String> = []
    private var alertedAnalog: Set<String> = []
    private var seen: Set<String> = []

    private var sweepTimer: Timer?
    private var dirty = false

    static let maxHistory = 1000          // MAX_DETECTION_HISTORY
    static let maxTrackPoints = 500
    static let maxEvents = 200
    static let carryForwardWindow: TimeInterval = 30
    static let activeWindow: TimeInterval = 30
    static let analogSilence: TimeInterval = 30

    init(notifier: AlertNotifier) {
        self.notifier = notifier
        aliases = Self.loadAliases()
        loadSession()
        sweepTimer = Timer.scheduledTimer(withTimeInterval: 10, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.sweep()
                self?.persistIfNeeded()
            }
        }
    }

    // MARK: - Ingest

    /// Entry point for a mesh text packet. One packet may hold several lines.
    func ingest(message: MeshTextMessage) {
        let senderName = senderNameResolver?(message.from)
        for update in MeshAlertParser.parse(message: message.text) {
            apply(update, at: message.rxTime, sender: message.from, senderName: senderName)
        }
    }

    /// Feed a raw line (demo feed / tests) with no Meshtastic envelope.
    func ingest(line: String, at time: Date = Date(), sender: UInt32? = nil) {
        for update in MeshAlertParser.parse(message: line) {
            apply(update, at: time, sender: sender, senderName: nil)
        }
    }

    /// The Swift twin of `update_detection()`.
    func apply(_ u: DetectionUpdate, at now: Date, sender: UInt32?, senderName: String?) {
        var det = detections[u.mac] ?? Detection(mac: u.mac, kind: u.kind, firstSeen: now, lastUpdate: now)
        if sender != nil { det.senderNodeNum = sender }
        if let senderName { det.senderName = senderName }

        if u.isPilotOnly {
            // `Pilot[mac]:` follows a `Drone:` line for the same MAC. Only the
            // operator position changes; RSSI / drone fix stay as they were.
            if let lat = u.pilotLat, let lon = u.pilotLon, lat != 0, lon != 0 {
                det.pilotLat = lat
                det.pilotLon = lon
            }
            det.lastUpdate = now
            det.status = .active
            detections[u.mac] = det
            evaluateAlerts(for: det, at: now)
            dirty = true
            return
        }

        det.kind = u.kind
        if let rssi = u.rssi { det.rssi = rssi }
        if let band = u.band { det.band = band }
        if let basicID = u.basicID, !basicID.isEmpty { det.basicID = basicID }   // else preserve previous
        if let nodeID = u.nodeID { det.nodeID = nodeID }

        let newLat = u.droneLat ?? 0
        let newLon = u.droneLon ?? 0
        var validDrone = newLat != 0 && newLon != 0

        if validDrone {
            det.droneLat = newLat
            det.droneLon = newLon
            det.lastGPSUpdate = now
            if let alt = u.droneAltitude { det.droneAltitude = alt }
            appendTrackPoint(&det, lat: newLat, lon: newLon, at: now)
        } else if u.kind != .analogFM {
            // Per-advert firmware splits BasicID / Location / System across
            // separate adverts. Carry the last real fix forward for up to 30 s,
            // then revert to no-GPS rather than freezing a stale position.
            if let fixAt = det.lastGPSUpdate, det.hasDroneGPS,
               now.timeIntervalSince(fixAt) <= Self.carryForwardWindow {
                validDrone = true
            } else {
                det.droneLat = 0
                det.droneLon = 0
            }
        }

        if let plat = u.pilotLat, let plon = u.pilotLon, plat != 0, plon != 0 {
            det.pilotLat = plat
            det.pilotLon = plon
        }

        if u.kind == .analogFM {
            det.freqMHz = u.freqMHz
            det.channel = u.channel
            det.rssiRaw = u.rssiRaw
            if let pos = nodePositionResolver?(u.nodeID, sender) {
                det.nodeLat = pos.lat
                det.nodeLon = pos.lon
            }
            if let raw = u.rssiRaw {
                det.radiusM = (RangeEstimator.maxRangeMeters(rssiRaw: raw, txDBm: settings.assumedVTXPowerDBm)).rounded()
            }
        }

        det.lastUpdate = now
        det.status = .active
        detections[u.mac] = det

        // analog_fm re-reports every ~5 s per channel; keep it out of the
        // history so it cannot evict real drone track points.
        if u.kind != .analogFM {
            history.append(HistoryRow(timestamp: now, alias: aliases[u.mac] ?? "", mac: u.mac,
                                      rssi: det.rssi, droneLat: validDrone ? det.droneLat : 0,
                                      droneLon: validDrone ? det.droneLon : 0,
                                      droneAltitude: det.droneAltitude, pilotLat: det.pilotLat,
                                      pilotLon: det.pilotLon, basicID: det.basicID ?? ""))
            if history.count > Self.maxHistory {
                history.removeFirst(history.count - Self.maxHistory)
            }
        }

        evaluateAlerts(for: det, at: now)
        dirty = true
    }

    private func appendTrackPoint(_ det: inout Detection, lat: Double, lon: Double, at now: Date) {
        if let last = det.track.last, abs(last.lat - lat) < 1e-6, abs(last.lon - lon) < 1e-6 {
            return
        }
        det.track.append(TrackPoint(lat: lat, lon: lon, time: now))
        if det.track.count > Self.maxTrackPoints {
            det.track.removeFirst(det.track.count - Self.maxTrackPoints)
        }
    }

    // MARK: - Alerts (should_trigger_webhook_earliest)

    private func evaluateAlerts(for det: Detection, at now: Date) {
        let mac = det.mac
        let isNew = !seen.contains(mac)
        seen.insert(mac)
        let name = det.displayName(aliases: aliases)

        switch det.kind {
        case .analogFM:
            guard settings.alertOnAnalogFM else { return }
            if !alertedAnalog.contains(mac) {
                alertedAnalog.insert(mac)
                var body = "\(name) rssi=\(det.rssiRaw ?? det.rssi ?? 0)"
                if let node = det.nodeID { body += " at node \(node)" }
                if let r = det.radiusM { body += " (≤ \(Int(r)) m)" }
                emit(mac: mac, title: "Analog FPV signal", body: body)
            }

        case .remoteID, .dji:
            let activeNow = det.hasDroneGPS && now.timeIntervalSince(det.lastUpdate) <= Self.activeWindow
            let wasActive = previousActive[mac] ?? false
            previousActive[mac] = activeNow

            if !wasActive && activeNow {
                let enabled = det.kind == .dji ? settings.alertOnDJI : settings.alertOnGPSDrones
                guard enabled else { return }
                let title = isNew && aliases[mac] == nil ? "New drone detected" : "Drone active"
                var body = "\(det.kind.label) \(name)"
                if let rssi = det.rssi { body += " RSSI \(rssi)" }
                body += String(format: " @ %.5f, %.5f", det.droneLat, det.droneLon)
                if det.hasPilotGPS { body += " · pilot located" }
                emit(mac: mac, title: title, body: body)
            } else if det.isNoGPSDrone && det.kind == .remoteID && det.basicID != "MAVLink"
                        && !alertedNoGPS.contains(mac) {
                // Same exclusions as the Python no-GPS webhook: not analog,
                // not DJI, not MAVLink. Fires once per detection session.
                guard settings.alertOnNoGPSDrones else { return }
                alertedNoGPS.insert(mac)
                var body = "\(name) heard with no GPS fix"
                if let rssi = det.rssi { body += " (RSSI \(rssi))" }
                if let sender = det.senderName { body += " via \(sender)" }
                emit(mac: mac, title: "Drone nearby", body: body)
            }
        }
    }

    private func emit(mac: String, title: String, body: String) {
        let event = AlertEvent(time: Date(), mac: mac, title: title, body: body)
        events.insert(event, at: 0)
        if events.count > Self.maxEvents { events.removeLast(events.count - Self.maxEvents) }
        notifier.post(title: title, body: body, thread: mac, identifier: "alert-\(mac)")
    }

    // MARK: - Ageing (cleanup_old_detections)

    func sweep(now: Date = Date()) {
        let stale = settings.staleThresholdSeconds
        var changed = false
        for (mac, original) in detections {
            var det = original
            let age = now.timeIntervalSince(det.lastUpdate)
            let newStatus: DetectionStatus
            if det.kind == .analogFM {
                // FPV nodes re-report every ~5 s; 30 s silence = signal gone.
                newStatus = age > Self.analogSilence ? .inactive : .active
            } else if age > stale * 30 {
                newStatus = .inactiveOld
            } else if age > stale * 3 {
                newStatus = .inactive
            } else {
                newStatus = .active
            }
            if newStatus != det.status {
                det.status = newStatus
                detections[mac] = det
                changed = true
            }
            if age > Self.activeWindow {
                // Let a returning emitter alert again.
                alertedNoGPS.remove(mac)
                alertedAnalog.remove(mac)
                previousActive[mac] = false
            }
        }
        if changed { dirty = true }
    }

    // MARK: - Queries

    var sortedDetections: [Detection] {
        detections.values.sorted { $0.lastUpdate > $1.lastUpdate }
    }

    var activeDetections: [Detection] {
        sortedDetections.filter { $0.status == .active }
    }

    var inactiveDetections: [Detection] {
        sortedDetections.filter { $0.status != .active }
    }

    func setAlias(_ alias: String, for mac: String) {
        let trimmed = alias.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            aliases.removeValue(forKey: mac)
        } else {
            aliases[mac] = trimmed
        }
    }

    func clearSession() {
        detections.removeAll()
        history.removeAll()
        events.removeAll()
        previousActive.removeAll()
        alertedNoGPS.removeAll()
        alertedAnalog.removeAll()
        seen.removeAll()
        dirty = true
        persistIfNeeded()
    }

    // MARK: - Export

    /// CSV with the same header as the desktop mapper's session file.
    func exportCSV() throws -> URL {
        let iso = ISO8601DateFormatter()
        var lines = ["timestamp,alias,mac,rssi,drone_lat,drone_long,drone_altitude,pilot_lat,pilot_long,basic_id"]
        for r in history {
            let fields: [String] = [
                iso.string(from: r.timestamp), r.alias, r.mac,
                r.rssi.map { String($0) } ?? "",
                String(r.droneLat), String(r.droneLon), String(r.droneAltitude),
                String(r.pilotLat), String(r.pilotLon), r.basicID,
            ]
            lines.append(fields.map(Self.csvEscape).joined(separator: ","))
        }
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("detections_\(Int(Date().timeIntervalSince1970)).csv")
        try lines.joined(separator: "\n").appending("\n").write(to: url, atomically: true, encoding: .utf8)
        return url
    }

    private static func csvEscape(_ s: String) -> String {
        if s.contains(",") || s.contains("\"") || s.contains("\n") {
            return "\"" + s.replacingOccurrences(of: "\"", with: "\"\"") + "\""
        }
        return s
    }

    // MARK: - Persistence

    private struct SessionFile: Codable {
        var detections: [String: Detection]
        var history: [HistoryRow]
        var events: [AlertEvent]
    }

    private static var sessionURL: URL {
        let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("session.json")
    }

    private static let aliasesKey = "mapper.aliases"

    private static func loadAliases() -> [String: String] {
        UserDefaults.standard.dictionary(forKey: aliasesKey) as? [String: String] ?? [:]
    }

    private func persistAliases() {
        UserDefaults.standard.set(aliases, forKey: Self.aliasesKey)
    }

    private func loadSession() {
        guard let data = try? Data(contentsOf: Self.sessionURL),
              let file = try? JSONDecoder().decode(SessionFile.self, from: data) else { return }
        detections = file.detections
        history = file.history
        events = file.events
        seen = Set(detections.keys)
        sweep()
    }

    /// Called from the sweep timer and when the app is backgrounded, so data
    /// collected while suspended survives a relaunch.
    func persistIfNeeded() {
        guard dirty else { return }
        dirty = false
        let file = SessionFile(detections: detections, history: history, events: events)
        if let data = try? JSONEncoder().encode(file) {
            try? data.write(to: Self.sessionURL, options: .atomic)
        }
    }
}
