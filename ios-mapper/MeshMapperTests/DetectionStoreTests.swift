import XCTest
@testable import MeshMapper

@MainActor
final class DetectionStoreTests: XCTestCase {

    private var store: DetectionStore!
    private let t0 = Date(timeIntervalSince1970: 1_700_000_000)

    override func setUp() async throws {
        store = DetectionStore(notifier: AlertNotifier())
        store.clearSession()
        store.nodePositionResolver = { nodeID, _ in
            nodeID == "RX01" ? (47.62, -122.34) : nil
        }
    }

    func testDroneThenPilotMergeIntoOneEntry() {
        store.ingest(line: "Drone: 60:60:1f:3a:2b:1c RSSI:-62 https://maps.google.com/?q=47.620500,-122.349300", at: t0)
        store.ingest(line: "Pilot[60:60:1f:3a:2b:1c]: https://maps.google.com/?q=47.618900,-122.352100", at: t0.addingTimeInterval(1))
        XCTAssertEqual(store.detections.count, 1)
        let det = store.detections["60:60:1f:3a:2b:1c"]!
        XCTAssertEqual(det.rssi, -62)
        XCTAssertEqual(det.droneLat, 47.6205, accuracy: 1e-9)
        XCTAssertEqual(det.pilotLat, 47.6189, accuracy: 1e-9)
        XCTAssertTrue(det.hasDroneGPS)
        XCTAssertTrue(det.hasPilotGPS)
        XCTAssertFalse(det.isNoGPSDrone)
        XCTAssertEqual(det.track.count, 1)
    }

    func testCarryForwardKeepsFixFor30Seconds() {
        let mac = "60:60:1f:3a:2b:1c"
        store.ingest(line: "Drone: \(mac) RSSI:-62 https://maps.google.com/?q=47.620500,-122.349300", at: t0)
        store.ingest(line: "Drone: \(mac) RSSI:-64", at: t0.addingTimeInterval(20))
        XCTAssertTrue(store.detections[mac]!.hasDroneGPS, "fix carried forward inside the 30 s window")
        XCTAssertEqual(store.detections[mac]!.rssi, -64)

        store.ingest(line: "Drone: \(mac) RSSI:-66", at: t0.addingTimeInterval(45))
        XCTAssertFalse(store.detections[mac]!.hasDroneGPS, "fix expires after 30 s without a real GPS advert")
        XCTAssertTrue(store.detections[mac]!.isNoGPSDrone)
    }

    func testBasicIDIsPreservedWhenLaterAdvertOmitsIt() {
        let mac = "a4:cf:12:9e:00:01"
        store.ingest(line: #"{"mac":"\#(mac)","rssi":-66,"drone_lat":47.612,"drone_long":-122.331,"basic_id":"1596F3B9"}"#, at: t0)
        store.ingest(line: #"{"mac":"\#(mac)","rssi":-60,"drone_lat":47.613,"drone_long":-122.332,"basic_id":""}"#, at: t0.addingTimeInterval(2))
        XCTAssertEqual(store.detections[mac]?.basicID, "1596F3B9")
        XCTAssertEqual(store.detections[mac]?.track.count, 2)
    }

    func testAnalogFMGetsNodePositionAndRing() {
        store.ingest(line: "AnalogFM: R3 5732MHz rssi=812 [RX01]", at: t0)
        let det = store.detections["af:00:16:64:52:03"]!
        XCTAssertEqual(det.kind, .analogFM)
        XCTAssertEqual(det.nodeLat ?? 0, 47.62, accuracy: 1e-9)
        XCTAssertNotNil(det.radiusM)
        XCTAssertEqual(det.radiusM ?? 0, RangeEstimator.maxRangeMeters(rssiRaw: 812).rounded(), accuracy: 0.5)
        XCTAssertFalse(det.isNoGPSDrone, "analog FM must never be treated as a no-GPS drone")
        XCTAssertTrue(store.history.isEmpty, "analog FM is not written to the history log")
    }

    func testAnalogFMWithoutKnownNodeHasNoRing() {
        store.ingest(line: "AnalogFM: A1 5865MHz rssi=650 [RX99]", at: t0)
        let det = store.detections.values.first!
        XCTAssertNil(det.nodeLat)
        XCTAssertNil(det.nodeCoordinate)
    }

    func testSweepAgesDetections() {
        store.settings.staleThresholdSeconds = 60
        store.ingest(line: "Drone: 8c:1f:64:12:34:56 RSSI:-81", at: t0)
        store.ingest(line: "AnalogFM: R3 5732MHz rssi=812 [RX01]", at: t0)

        store.sweep(now: t0.addingTimeInterval(31))
        XCTAssertEqual(store.detections["af:00:16:64:52:03"]?.status, .inactive, "analog drops after 30 s silence")
        XCTAssertEqual(store.detections["8c:1f:64:12:34:56"]?.status, .active)

        store.sweep(now: t0.addingTimeInterval(60 * 3 + 1))
        XCTAssertEqual(store.detections["8c:1f:64:12:34:56"]?.status, .inactive)

        store.sweep(now: t0.addingTimeInterval(60 * 30 + 1))
        XCTAssertEqual(store.detections["8c:1f:64:12:34:56"]?.status, .inactiveOld)
    }

    func testAlertsFireOnActivationAndOncePerSessionForNoGPS() {
        store.ingest(line: "Drone: 60:60:1f:3a:2b:1c RSSI:-62 https://maps.google.com/?q=47.620500,-122.349300", at: t0)
        XCTAssertEqual(store.events.count, 1)
        XCTAssertEqual(store.events.first?.title, "New drone detected")

        // Same drone again while active: no second alert.
        store.ingest(line: "Drone: 60:60:1f:3a:2b:1c RSSI:-60 https://maps.google.com/?q=47.621000,-122.349000", at: t0.addingTimeInterval(5))
        XCTAssertEqual(store.events.count, 1)

        // No-GPS drone alerts once...
        store.ingest(line: "Drone: 8c:1f:64:12:34:56 RSSI:-81", at: t0)
        store.ingest(line: "Drone: 8c:1f:64:12:34:56 RSSI:-80", at: t0.addingTimeInterval(3))
        XCTAssertEqual(store.events.count, 2)
        XCTAssertEqual(store.events.first?.title, "Drone nearby")

        // ...and again after it has been silent for more than 30 s.
        store.sweep(now: t0.addingTimeInterval(40))
        store.ingest(line: "Drone: 8c:1f:64:12:34:56 RSSI:-79", at: t0.addingTimeInterval(41))
        XCTAssertEqual(store.events.count, 3)
    }

    func testMAVLinkNoGPSDoesNotAlert() {
        store.ingest(line: #"{"mac":"aa:bb:cc:dd:ee:01","rssi":-70,"basic_id":"MAVLink"}"#, at: t0)
        XCTAssertTrue(store.events.isEmpty)
    }

    func testAnalogAlertRespectsToggle() {
        store.settings.alertOnAnalogFM = false
        store.ingest(line: "AnalogFM: R3 5732MHz rssi=812 [RX01]", at: t0)
        XCTAssertTrue(store.events.isEmpty)
        store.settings.alertOnAnalogFM = true
        store.ingest(line: "AnalogFM: A1 5865MHz rssi=650 [RX01]", at: t0)
        XCTAssertEqual(store.events.count, 1)
    }

    func testCSVExportHasMapperHeader() throws {
        store.ingest(line: "Drone: 60:60:1f:3a:2b:1c RSSI:-62 https://maps.google.com/?q=47.620500,-122.349300", at: t0)
        store.setAlias("Test Drone", for: "60:60:1f:3a:2b:1c")
        let url = try store.exportCSV()
        let text = try String(contentsOf: url, encoding: .utf8)
        let lines = text.split(separator: "\n")
        XCTAssertEqual(lines.first, "timestamp,alias,mac,rssi,drone_lat,drone_long,drone_altitude,pilot_lat,pilot_long,basic_id")
        XCTAssertEqual(lines.count, 2)
        XCTAssertTrue(lines[1].contains("60:60:1f:3a:2b:1c,-62,47.6205,-122.3493"))
    }
}
