import Foundation

/// Minimal protocol-buffers wire-format reader/writer.
///
/// The app only needs a handful of fields from the Meshtastic `FromRadio`
/// stream (text packets, node info, positions, config-complete) and to write
/// a single `ToRadio.want_config_id`, so a hand-rolled decoder avoids pulling
/// the full generated Meshtastic protobuf package into a prototype.
/// Field numbers live in `MeshtasticMessages.swift`.
enum ProtoWireType: Int {
    case varint = 0
    case fixed64 = 1
    case lengthDelimited = 2
    case startGroup = 3
    case endGroup = 4
    case fixed32 = 5
}

enum ProtoValue {
    case varint(UInt64)
    case fixed64(UInt64)
    case bytes(Data)
    case fixed32(UInt32)

    var uint64: UInt64? {
        if case .varint(let v) = self { return v }
        return nil
    }
    var uint32: UInt32? {
        switch self {
        case .varint(let v): return UInt32(truncatingIfNeeded: v)
        case .fixed32(let v): return v
        default: return nil
        }
    }
    var int32: Int32? {
        switch self {
        case .varint(let v): return Int32(truncatingIfNeeded: v)
        case .fixed32(let v): return Int32(bitPattern: v)
        default: return nil
        }
    }
    var float: Float? {
        if case .fixed32(let v) = self { return Float(bitPattern: v) }
        return nil
    }
    var bool: Bool? { uint64.map { $0 != 0 } }
    var data: Data? {
        if case .bytes(let d) = self { return d }
        return nil
    }
    var string: String? { data.flatMap { String(data: $0, encoding: .utf8) } }
}

struct ProtoField {
    let number: Int
    let value: ProtoValue
}

enum ProtoError: Error {
    case truncated
    case malformedVarint
    case unsupportedWireType(Int)
}

struct ProtoReader {
    private let bytes: [UInt8]
    private var pos: Int = 0

    init(_ data: Data) {
        bytes = [UInt8](data)
    }

    var isAtEnd: Bool { pos >= bytes.count }

    /// Decode every top-level field in the buffer. Unknown fields are returned
    /// too; callers switch on `number` and ignore what they do not need.
    static func fields(of data: Data) throws -> [ProtoField] {
        var reader = ProtoReader(data)
        var out: [ProtoField] = []
        while let f = try reader.next() {
            out.append(f)
        }
        return out
    }

    mutating func next() throws -> ProtoField? {
        if isAtEnd { return nil }
        let key = try readVarint()
        let number = Int(key >> 3)
        let wireRaw = Int(key & 0x7)
        guard let wire = ProtoWireType(rawValue: wireRaw) else {
            throw ProtoError.unsupportedWireType(wireRaw)
        }
        switch wire {
        case .varint:
            return ProtoField(number: number, value: .varint(try readVarint()))
        case .fixed64:
            return ProtoField(number: number, value: .fixed64(try readFixed64()))
        case .lengthDelimited:
            let len = Int(try readVarint())
            guard len >= 0, pos + len <= bytes.count else { throw ProtoError.truncated }
            let d = Data(bytes[pos..<(pos + len)])
            pos += len
            return ProtoField(number: number, value: .bytes(d))
        case .fixed32:
            return ProtoField(number: number, value: .fixed32(try readFixed32()))
        case .startGroup, .endGroup:
            throw ProtoError.unsupportedWireType(wireRaw)
        }
    }

    private mutating func readVarint() throws -> UInt64 {
        var result: UInt64 = 0
        var shift: UInt64 = 0
        while true {
            guard pos < bytes.count else { throw ProtoError.truncated }
            let b = bytes[pos]
            pos += 1
            result |= UInt64(b & 0x7F) << shift
            if b & 0x80 == 0 { return result }
            shift += 7
            if shift > 63 { throw ProtoError.malformedVarint }
        }
    }

    private mutating func readFixed32() throws -> UInt32 {
        guard pos + 4 <= bytes.count else { throw ProtoError.truncated }
        var v: UInt32 = 0
        for i in 0..<4 { v |= UInt32(bytes[pos + i]) << (8 * UInt32(i)) }
        pos += 4
        return v
    }

    private mutating func readFixed64() throws -> UInt64 {
        guard pos + 8 <= bytes.count else { throw ProtoError.truncated }
        var v: UInt64 = 0
        for i in 0..<8 { v |= UInt64(bytes[pos + i]) << (8 * UInt64(i)) }
        pos += 8
        return v
    }
}

struct ProtoWriter {
    private(set) var data = Data()

    mutating func varint(field: Int, _ value: UInt64) {
        writeVarint(UInt64(field << 3) | UInt64(ProtoWireType.varint.rawValue))
        writeVarint(value)
    }

    mutating func bytes(field: Int, _ payload: Data) {
        writeVarint(UInt64(field << 3) | UInt64(ProtoWireType.lengthDelimited.rawValue))
        writeVarint(UInt64(payload.count))
        data.append(payload)
    }

    mutating func fixed32(field: Int, _ value: UInt32) {
        writeVarint(UInt64(field << 3) | UInt64(ProtoWireType.fixed32.rawValue))
        for i in 0..<4 {
            data.append(UInt8((value >> (8 * UInt32(i))) & 0xFF))
        }
    }

    private mutating func writeVarint(_ value: UInt64) {
        var v = value
        repeat {
            var byte = UInt8(v & 0x7F)
            v >>= 7
            if v != 0 { byte |= 0x80 }
            data.append(byte)
        } while v != 0
    }
}
