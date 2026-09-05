import XCTest
@testable import MeshMapper

final class MeshAlertParserTests: XCTestCase {

    func testDroneLineWithGPS() {
        let u = MeshAlertParser.parse(line: "Drone: 60:60:1F:3A:2B:1C RSSI:-62 https://maps.google.com/?q=47.620500,-122.349300")
        XCTAssertNotNil(u)
        XCTAssertEqual(u?.mac, "60:60:1f:3a:2b:1c")
        XCTAssertEqual(u?.kind, .remoteID)
        XCTAssertEqual(u?.rssi, -62)
        XCTAssertNil(u?.band)
        XCTAssertEqual(u?.droneLat, 47.6205, accuracy: 1e-9)
        XCTAssertEqual(u?.droneLon, -122.3493, accuracy: 1e-9)
        XCTAssertFalse(u?.isPilotOnly ?? true)
    }

    func testDroneLineWithoutGPS() {
        let u = MeshAlertParser.parse(line: "Drone: 8c:1f:64:12:34:56 RSSI:-81")
        XCTAssertEqual(u?.mac, "8c:1f:64:12:34:56")
        XCTAssertEqual(u?.rssi, -81)
        XCTAssertNil(u?.droneLat)
        XCTAssertNil(u?.droneLon)
    }

    func testC5DroneLineCarriesBand() {
        let u = MeshAlertParser.parse(line: "Drone[5G]: 34:d2:62:aa:bb:cc RSSI:-70 https://maps.google.com/?q=47.628100,-122.342000")
        XCTAssertEqual(u?.band, "5G")
        XCTAssertEqual(u?.mac, "34:d2:62:aa:bb:cc")
        XCTAssertEqual(u?.droneLat, 47.6281, accuracy: 1e-9)
    }

    func testPilotLine() {
        let u = MeshAlertParser.parse(line: "Pilot[60:60:1f:3a:2b:1c]: https://maps.google.com/?q=47.618900,-122.352100")
        XCTAssertEqual(u?.mac, "60:60:1f:3a:2b:1c")
        XCTAssertTrue(u?.isPilotOnly ?? false)
        XCTAssertEqual(u?.pilotLat, 47.6189, accuracy: 1e-9)
        XCTAssertEqual(u?.pilotLon, -122.3521, accuracy: 1e-9)
        XCTAssertNil(u?.rssi)
    }

    func testDJILineNormalizesBareMAC() {
        let u = MeshAlertParser.parse(line: "DJI 60601F9A8B7C RSSI:-58 https://maps.google.com/?q=47.615200,-122.338700")
        XCTAssertEqual(u?.kind, .dji)
        XCTAssertEqual(u?.mac, "60:60:1f:9a:8b:7c")
        XCTAssertEqual(u?.rssi, -58)
        XCTAssertEqual(u?.droneLon, -122.3387, accuracy: 1e-9)
    }

    func testDJILineWithoutGPS() {
        let u = MeshAlertParser.parse(line: "DJI 60601f9a8b7c RSSI:-58")
        XCTAssertEqual(u?.kind, .dji)
        XCTAssertNil(u?.droneLat)
    }

    func testAnalogFMLine() {
        let u = MeshAlertParser.parse(line: "AnalogFM: R3 5732MHz rssi=812 [RX01]")
        XCTAssertEqual(u?.kind, .analogFM)
        XCTAssertEqual(u?.band, "R")
        XCTAssertEqual(u?.channel, 3)
        XCTAssertEqual(u?.freqMHz, 5732)
        XCTAssertEqual(u?.rssiRaw, 812)
        XCTAssertEqual(u?.rssi, 812)
        XCTAssertEqual(u?.nodeID, "RX01")
        XCTAssertEqual(u?.basicID, "5.8G-R3-5732MHz")
        // channel_to_mac(): AF:00:<freq>>8:<freq&0xff>:<band char>:<ch>
        XCTAssertEqual(u?.mac, "af:00:16:64:52:03")
    }

    func testAnalogFMMACMatchesFirmwareJSON() {
        // The RX5808 JSON form of the same channel must land on the same key.
        let json = MeshAlertParser.parse(line: #"{"type":"analog_fm","mac":"AF:00:16:64:52:03","freq_mhz":5732,"band":"R","ch":3,"rssi_raw":812,"rssi":812,"basic_id":"5.8G-R3-5732MHz","node_id":"RX01"}"#)
        let text = MeshAlertParser.parse(line: "AnalogFM: R3 5732MHz rssi=812 [RX01]")
        XCTAssertEqual(json?.mac, text?.mac)
        XCTAssertEqual(json?.kind, .analogFM)
        XCTAssertEqual(json?.rssiRaw, 812)
        XCTAssertEqual(json?.channel, 3)
        XCTAssertEqual(json?.nodeID, "RX01")
    }

    func testFullJSONDetection() {
        let u = MeshAlertParser.parse(line: #"{"mac":"a4:cf:12:9e:00:01","rssi":-66,"drone_lat":47.612,"drone_long":-122.331,"drone_altitude":95,"pilot_lat":47.6105,"pilot_long":-122.333,"basic_id":"1596F3B9A2C1D4E5F6"}"#)
        XCTAssertEqual(u?.kind, .remoteID)
        XCTAssertEqual(u?.mac, "a4:cf:12:9e:00:01")
        XCTAssertEqual(u?.rssi, -66)
        XCTAssertEqual(u?.droneAltitude, 95)
        XCTAssertEqual(u?.pilotLat, 47.6105, accuracy: 1e-9)
        XCTAssertEqual(u?.basicID, "1596F3B9A2C1D4E5F6")
    }

    func testDJIJSONUsesIDType() {
        let u = MeshAlertParser.parse(line: #"{"mac":"60:60:1f:9a:8b:7c","rssi":-58,"id_type":"DJI","basic_id":"0AXBC1234","drone_lat":1.5,"drone_long":2.5,"drone_altitude":10,"height":5,"home_lat":0,"home_long":0,"pilot_lat":1.4,"pilot_long":2.4,"product_type":58}"#)
        XCTAssertEqual(u?.kind, .dji)
        XCTAssertEqual(u?.basicID, "0AXBC1234")
    }

    func testRemoteIDKeyIsNormalized() {
        let u = MeshAlertParser.parse(line: #"{"mac":"aa:bb:cc:dd:ee:ff","rssi":-50,"remote_id":"ABC"}"#)
        XCTAssertEqual(u?.basicID, "ABC")
    }

    func testJSONWithSurroundingNoise() {
        let u = MeshAlertParser.parse(line: #"[HOME] {"mac":"aa:bb:cc:dd:ee:ff","rssi":-50,"basic_id":"X"} trailing"#)
        XCTAssertEqual(u?.mac, "aa:bb:cc:dd:ee:ff")
    }

    func testHeartbeatAndStatusFramesAreIgnored() {
        XCTAssertNil(MeshAlertParser.parse(line: #"{"heartbeat":"home_node active","tracked_drones":2}"#))
        XCTAssertNil(MeshAlertParser.parse(line: #"{"status":"active","mac":"aa:bb:cc:dd:ee:ff"}"#))
        XCTAssertNil(MeshAlertParser.parse(line: #"{"info":"RX5808 scanner ready","node_id":"RX01","channels":40,"threshold":600}"#))
    }

    func testUnrelatedChatterIsIgnored() {
        XCTAssertNil(MeshAlertParser.parse(line: "hello from the mesh"))
        XCTAssertNil(MeshAlertParser.parse(line: "WATCHDOG_RESET"))
        XCTAssertNil(MeshAlertParser.parse(line: "[SCAN] channel 12 rejected by regulatory domain"))
        XCTAssertNil(MeshAlertParser.parse(line: ""))
    }

    func testMultiLineMessage() {
        let updates = MeshAlertParser.parse(message:
            "Drone: 60:60:1f:3a:2b:1c RSSI:-62 https://maps.google.com/?q=47.620500,-122.349300\n" +
            "Pilot[60:60:1f:3a:2b:1c]: https://maps.google.com/?q=47.618900,-122.352100\r\n")
        XCTAssertEqual(updates.count, 2)
        XCTAssertFalse(updates[0].isPilotOnly)
        XCTAssertTrue(updates[1].isPilotOnly)
        XCTAssertEqual(updates[0].mac, updates[1].mac)
    }

    func testNormalizeMAC() {
        XCTAssertEqual(MeshAlertParser.normalizeMAC("AA:BB:CC:DD:EE:FF"), "aa:bb:cc:dd:ee:ff")
        XCTAssertEqual(MeshAlertParser.normalizeMAC("aabbccddeeff"), "aa:bb:cc:dd:ee:ff")
        XCTAssertEqual(MeshAlertParser.normalizeMAC("AA-BB-CC-DD-EE-FF"), "aa:bb:cc:dd:ee:ff")
        XCTAssertEqual(MeshAlertParser.normalizeMAC("not-a-mac"), "not-a-mac")
    }
}
