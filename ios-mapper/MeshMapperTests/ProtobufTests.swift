import XCTest
@testable import MeshMapper

final class ProtobufTests: XCTestCase {

    func testWantConfigEncoding() {
        // field 3, varint → tag 0x18; 300 → 0xAC 0x02
        XCTAssertEqual([UInt8](ToRadio.wantConfig(id: 300)), [0x18, 0xAC, 0x02])
    }

    func testHeartbeatEncoding() {
        // field 7, length-delimited, empty → tag 0x3A, len 0
        XCTAssertEqual([UInt8](ToRadio.heartbeat()), [0x3A, 0x00])
    }

    func testVarintRoundTrip() throws {
        var w = ProtoWriter()
        w.varint(field: 1, 0)
        w.varint(field: 2, 127)
        w.varint(field: 3, 128)
        w.varint(field: 4, UInt64(UInt32.max))
        let fields = try ProtoReader.fields(of: w.data)
        XCTAssertEqual(fields.map { $0.number }, [1, 2, 3, 4])
        XCTAssertEqual(fields[0].value.uint64, 0)
        XCTAssertEqual(fields[1].value.uint64, 127)
        XCTAssertEqual(fields[2].value.uint64, 128)
        XCTAssertEqual(fields[3].value.uint32, UInt32.max)
    }

    func testTruncatedBufferThrows() {
        // length-delimited field claiming 10 bytes but only 2 present
        XCTAssertThrowsError(try ProtoReader.fields(of: Data([0x12, 0x0A, 0x01, 0x02])))
    }

    /// Hand-assemble FromRadio { packet { from, to, decoded { portnum TEXT, payload }, rx_time, rx_rssi } }
    func testDecodeTextPacket() throws {
        let text = "Drone: 60:60:1f:3a:2b:1c RSSI:-62"

        var data = ProtoWriter()
        data.varint(field: MeshtasticProto.dataPortnum, UInt64(MeshtasticProto.portTextMessage))
        data.bytes(field: MeshtasticProto.dataPayload, Data(text.utf8))

        var packet = ProtoWriter()
        packet.fixed32(field: MeshtasticProto.packetFrom, 0x1234_5678)
        packet.fixed32(field: MeshtasticProto.packetTo, MeshtasticProto.broadcastAddr)
        packet.varint(field: MeshtasticProto.packetChannel, 0)
        packet.bytes(field: MeshtasticProto.packetDecoded, data.data)
        packet.fixed32(field: MeshtasticProto.packetRxTime, 1_700_000_000)
        packet.varint(field: MeshtasticProto.packetRxRSSI, UInt64(bitPattern: Int64(-95)))

        var fromRadio = ProtoWriter()
        fromRadio.bytes(field: MeshtasticProto.fromRadioPacket, packet.data)

        guard case .packet(let p) = try FromRadio.decode(fromRadio.data) else {
            return XCTFail("expected a packet")
        }
        XCTAssertEqual(p.from, 0x1234_5678)
        XCTAssertEqual(p.to, MeshtasticProto.broadcastAddr)
        XCTAssertEqual(p.rxTime, 1_700_000_000)
        XCTAssertEqual(p.rxRSSI, -95)
        XCTAssertEqual(p.textMessage, text)
    }

    func testEncryptedPacketHasNoDecoded() throws {
        var packet = ProtoWriter()
        packet.fixed32(field: MeshtasticProto.packetFrom, 1)
        packet.bytes(field: 5, Data([0xDE, 0xAD]))   // encrypted
        var fromRadio = ProtoWriter()
        fromRadio.bytes(field: MeshtasticProto.fromRadioPacket, packet.data)
        guard case .packet(let p) = try FromRadio.decode(fromRadio.data) else {
            return XCTFail("expected a packet")
        }
        XCTAssertNil(p.decoded)
        XCTAssertNil(p.textMessage)
    }

    func testDecodeNodeInfoWithNegativeLongitude() throws {
        var user = ProtoWriter()
        user.bytes(field: MeshtasticProto.userID, Data("!deadbeef".utf8))
        user.bytes(field: MeshtasticProto.userLongName, Data("RX01 Rooftop".utf8))
        user.bytes(field: MeshtasticProto.userShortName, Data("RX01".utf8))

        var pos = ProtoWriter()
        pos.fixed32(field: MeshtasticProto.positionLatitudeI, UInt32(bitPattern: 476_205_000))
        pos.fixed32(field: MeshtasticProto.positionLongitudeI, UInt32(bitPattern: -1_223_493_000))
        pos.varint(field: MeshtasticProto.positionAltitude, UInt64(bitPattern: Int64(-3)))

        var info = ProtoWriter()
        info.varint(field: MeshtasticProto.nodeInfoNum, 0xDEAD_BEEF)
        info.bytes(field: MeshtasticProto.nodeInfoUser, user.data)
        info.bytes(field: MeshtasticProto.nodeInfoPosition, pos.data)
        info.fixed32(field: MeshtasticProto.nodeInfoLastHeard, 1_700_000_000)

        var fromRadio = ProtoWriter()
        fromRadio.bytes(field: MeshtasticProto.fromRadioNodeInfo, info.data)

        guard case .nodeInfo(let n) = try FromRadio.decode(fromRadio.data) else {
            return XCTFail("expected node info")
        }
        XCTAssertEqual(n.num, 0xDEAD_BEEF)
        XCTAssertEqual(n.user?.shortName, "RX01")
        XCTAssertEqual(n.user?.longName, "RX01 Rooftop")
        XCTAssertEqual(n.position?.latitude ?? 0, 47.6205, accuracy: 1e-7)
        XCTAssertEqual(n.position?.longitude ?? 0, -122.3493, accuracy: 1e-7)
        XCTAssertEqual(n.position?.altitude, -3)
        XCTAssertEqual(n.lastHeard, 1_700_000_000)
    }

    func testConfigCompleteAndMyInfo() throws {
        var complete = ProtoWriter()
        complete.varint(field: MeshtasticProto.fromRadioConfigCompleteID, 42)
        guard case .configComplete(let id) = try FromRadio.decode(complete.data) else {
            return XCTFail("expected config complete")
        }
        XCTAssertEqual(id, 42)

        var my = ProtoWriter()
        my.varint(field: MeshtasticProto.myNodeInfoNum, 7)
        var fromRadio = ProtoWriter()
        fromRadio.bytes(field: MeshtasticProto.fromRadioMyInfo, my.data)
        guard case .myInfo(let num) = try FromRadio.decode(fromRadio.data) else {
            return XCTFail("expected my info")
        }
        XCTAssertEqual(num, 7)
    }

    func testUnknownFromRadioVariantIsOther() throws {
        var w = ProtoWriter()
        w.bytes(field: 13, Data([0x01]))   // metadata
        guard case .other(let field) = try FromRadio.decode(w.data) else {
            return XCTFail("expected other")
        }
        XCTAssertEqual(field, 13)
    }
}
