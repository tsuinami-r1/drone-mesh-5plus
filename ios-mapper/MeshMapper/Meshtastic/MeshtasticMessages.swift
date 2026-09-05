import Foundation

/// The subset of Meshtastic protobuf messages the app decodes, with field
/// numbers taken from `meshtastic/mesh.proto` and `meshtastic/portnums.proto`.
enum MeshtasticProto {
    // FromRadio
    static let fromRadioPacket = 2          // MeshPacket
    static let fromRadioMyInfo = 3          // MyNodeInfo
    static let fromRadioNodeInfo = 4        // NodeInfo
    static let fromRadioConfigCompleteID = 7 // uint32

    // ToRadio
    static let toRadioWantConfigID = 3      // uint32
    static let toRadioHeartbeat = 7         // Heartbeat (empty message)

    // MeshPacket
    static let packetFrom = 1               // fixed32
    static let packetTo = 2                 // fixed32
    static let packetChannel = 3            // uint32
    static let packetDecoded = 4            // Data
    static let packetID = 6                 // fixed32
    static let packetRxTime = 7             // fixed32
    static let packetRxSNR = 8              // float
    static let packetHopLimit = 9           // uint32
    static let packetRxRSSI = 12            // int32
    static let packetHopStart = 15          // uint32

    // Data
    static let dataPortnum = 1              // enum
    static let dataPayload = 2              // bytes

    // NodeInfo
    static let nodeInfoNum = 1
    static let nodeInfoUser = 2
    static let nodeInfoPosition = 3
    static let nodeInfoLastHeard = 5        // fixed32

    // User
    static let userID = 1
    static let userLongName = 2
    static let userShortName = 3

    // Position
    static let positionLatitudeI = 1        // sfixed32, degrees * 1e7
    static let positionLongitudeI = 2       // sfixed32
    static let positionAltitude = 3         // int32 (varint)
    static let positionTime = 4             // fixed32

    // MyNodeInfo
    static let myNodeInfoNum = 1

    // PortNum values
    static let portTextMessage: UInt32 = 1
    static let portPosition: UInt32 = 3
    static let portNodeInfo: UInt32 = 4

    /// Broadcast destination node number.
    static let broadcastAddr: UInt32 = 0xFFFF_FFFF
}

struct MeshData {
    var portnum: UInt32 = 0
    var payload = Data()

    init(_ data: Data) throws {
        for f in try ProtoReader.fields(of: data) {
            switch f.number {
            case MeshtasticProto.dataPortnum: portnum = f.value.uint32 ?? 0
            case MeshtasticProto.dataPayload: payload = f.value.data ?? Data()
            default: break
            }
        }
    }
}

struct MeshPacket {
    var from: UInt32 = 0
    var to: UInt32 = 0
    var channel: UInt32 = 0
    var id: UInt32 = 0
    var rxTime: UInt32 = 0
    var rxSNR: Float = 0
    var rxRSSI: Int32 = 0
    var hopLimit: UInt32 = 0
    var hopStart: UInt32 = 0
    /// Absent when the radio could not decrypt the packet (unknown channel key).
    var decoded: MeshData?

    init(_ data: Data) throws {
        for f in try ProtoReader.fields(of: data) {
            switch f.number {
            case MeshtasticProto.packetFrom:     from = f.value.uint32 ?? 0
            case MeshtasticProto.packetTo:       to = f.value.uint32 ?? 0
            case MeshtasticProto.packetChannel:  channel = f.value.uint32 ?? 0
            case MeshtasticProto.packetID:       id = f.value.uint32 ?? 0
            case MeshtasticProto.packetRxTime:   rxTime = f.value.uint32 ?? 0
            case MeshtasticProto.packetRxSNR:    rxSNR = f.value.float ?? 0
            case MeshtasticProto.packetRxRSSI:   rxRSSI = f.value.int32 ?? 0
            case MeshtasticProto.packetHopLimit: hopLimit = f.value.uint32 ?? 0
            case MeshtasticProto.packetHopStart: hopStart = f.value.uint32 ?? 0
            case MeshtasticProto.packetDecoded:
                if let d = f.value.data { decoded = try MeshData(d) }
            default: break
            }
        }
    }

    var textMessage: String? {
        guard let decoded, decoded.portnum == MeshtasticProto.portTextMessage else { return nil }
        return String(data: decoded.payload, encoding: .utf8)
    }
}

struct MeshUser {
    var id = ""
    var longName = ""
    var shortName = ""

    init(_ data: Data) throws {
        for f in try ProtoReader.fields(of: data) {
            switch f.number {
            case MeshtasticProto.userID:        id = f.value.string ?? ""
            case MeshtasticProto.userLongName:  longName = f.value.string ?? ""
            case MeshtasticProto.userShortName: shortName = f.value.string ?? ""
            default: break
            }
        }
    }
}

struct MeshPosition {
    var latitudeI: Int32?
    var longitudeI: Int32?
    var altitude: Int32?
    var time: UInt32 = 0

    init(_ data: Data) throws {
        for f in try ProtoReader.fields(of: data) {
            switch f.number {
            case MeshtasticProto.positionLatitudeI:  latitudeI = f.value.int32
            case MeshtasticProto.positionLongitudeI: longitudeI = f.value.int32
            case MeshtasticProto.positionAltitude:   altitude = f.value.int32
            case MeshtasticProto.positionTime:       time = f.value.uint32 ?? 0
            default: break
            }
        }
    }

    var latitude: Double? { latitudeI.map { Double($0) * 1e-7 } }
    var longitude: Double? { longitudeI.map { Double($0) * 1e-7 } }
}

struct MeshNodeInfo {
    var num: UInt32 = 0
    var user: MeshUser?
    var position: MeshPosition?
    var lastHeard: UInt32 = 0

    init(_ data: Data) throws {
        for f in try ProtoReader.fields(of: data) {
            switch f.number {
            case MeshtasticProto.nodeInfoNum:       num = f.value.uint32 ?? 0
            case MeshtasticProto.nodeInfoLastHeard: lastHeard = f.value.uint32 ?? 0
            case MeshtasticProto.nodeInfoUser:
                if let d = f.value.data { user = try MeshUser(d) }
            case MeshtasticProto.nodeInfoPosition:
                if let d = f.value.data { position = try MeshPosition(d) }
            default: break
            }
        }
    }
}

enum FromRadio {
    case packet(MeshPacket)
    case myInfo(nodeNum: UInt32)
    case nodeInfo(MeshNodeInfo)
    case configComplete(id: UInt32)
    case other(field: Int)

    /// One `FromRadio` message decodes to exactly one variant (it is a oneof).
    static func decode(_ data: Data) throws -> FromRadio {
        for f in try ProtoReader.fields(of: data) {
            switch f.number {
            case MeshtasticProto.fromRadioPacket:
                if let d = f.value.data { return .packet(try MeshPacket(d)) }
            case MeshtasticProto.fromRadioMyInfo:
                if let d = f.value.data {
                    for g in try ProtoReader.fields(of: d) where g.number == MeshtasticProto.myNodeInfoNum {
                        return .myInfo(nodeNum: g.value.uint32 ?? 0)
                    }
                    return .myInfo(nodeNum: 0)
                }
            case MeshtasticProto.fromRadioNodeInfo:
                if let d = f.value.data { return .nodeInfo(try MeshNodeInfo(d)) }
            case MeshtasticProto.fromRadioConfigCompleteID:
                return .configComplete(id: f.value.uint32 ?? 0)
            default:
                return .other(field: f.number)
            }
        }
        return .other(field: 0)
    }
}

enum ToRadio {
    /// `ToRadio { want_config_id = id }` — asks the radio to stream its config
    /// and node DB, then a `config_complete_id` echo, then live packets.
    static func wantConfig(id: UInt32) -> Data {
        var w = ProtoWriter()
        w.varint(field: MeshtasticProto.toRadioWantConfigID, UInt64(id))
        return w.data
    }

    /// `ToRadio { heartbeat = {} }` — keeps the link marked alive on the radio.
    static func heartbeat() -> Data {
        var w = ProtoWriter()
        w.bytes(field: MeshtasticProto.toRadioHeartbeat, Data())
        return w.data
    }
}
