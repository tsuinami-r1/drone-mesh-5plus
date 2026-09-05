import Foundation
import CoreBluetooth
import Combine

/// A Meshtastic radio seen while scanning.
struct DiscoveredRadio: Identifiable, Equatable {
    let id: UUID
    let name: String
    let rssi: Int
}

/// A decrypted text packet received from the mesh.
struct MeshTextMessage {
    let text: String
    let from: UInt32
    let to: UInt32
    let channel: UInt32
    let rxTime: Date
    let rxSNR: Float
    let rxRSSI: Int32
}

enum RadioLinkState: Equatable {
    case bluetoothOff
    case unauthorized
    case idle
    case scanning
    case connecting(String)
    case configuring(String)
    case connected(String)

    var label: String {
        switch self {
        case .bluetoothOff:          return "Bluetooth off"
        case .unauthorized:          return "Bluetooth not authorized"
        case .idle:                  return "Not connected"
        case .scanning:              return "Scanning…"
        case .connecting(let n):     return "Connecting to \(n)…"
        case .configuring(let n):    return "Loading config from \(n)…"
        case .connected(let n):      return "Connected to \(n)"
        }
    }

    var isConnected: Bool {
        if case .connected = self { return true }
        return false
    }
}

/// CoreBluetooth client for the Meshtastic BLE API.
///
/// The iPhone talks to the radio the same way the official Meshtastic app
/// does: one GATT service with `toRadio` (write), `fromRadio` (read) and
/// `fromNum` (notify). Startup writes `ToRadio.want_config_id`, drains
/// `fromRadio` until it returns an empty value, and from then on every
/// `fromNum` notification triggers another drain. Text packets are handed
/// to `onTextMessage`; node-info and position packets keep a local node DB
/// so analog FM alerts can be pinned to the reporting node's position.
///
/// Background operation relies on the `bluetooth-central` background mode
/// plus CoreBluetooth state restoration: while the radio is connected, each
/// `fromNum` notification wakes the app for a few seconds even when it is
/// suspended, and iOS relaunches the app on the next BLE event if it was
/// terminated for memory. A pending `connect()` survives in the background
/// too, so a radio that drops out of range is re-joined automatically when
/// it comes back.
///
/// Only one BLE central can hold a Meshtastic radio at a time: this app and
/// the official Meshtastic app cannot both be connected to the same radio.
///
/// The central manager is created with `queue: nil`, so every delegate
/// callback already arrives on the main queue; the `nonisolated` delegate
/// shims below use `MainActor.assumeIsolated` to hop into this class's
/// isolation without a dispatch.
@MainActor
final class MeshtasticRadio: NSObject, ObservableObject {

    static let restoreIdentifier = "MeshMapper.central"
    static let serviceUUID   = CBUUID(string: "6ba1b218-15a8-461f-9fa8-5dcae273eafd")
    static let toRadioUUID   = CBUUID(string: "f75c76d2-129e-4dad-a1dd-7866124401e7")
    static let fromRadioUUID = CBUUID(string: "2c55e69e-4993-11ed-b878-0242ac120002")
    static let fromNumUUID   = CBUUID(string: "ed9da18c-a800-4f66-a670-aa7547e34453")

    private static let savedPeripheralKey = "meshtastic.peripheral.uuid"
    private static let heartbeatInterval: TimeInterval = 300
    private static let logLimit = 200

    @Published private(set) var state: RadioLinkState = .idle
    @Published private(set) var discovered: [DiscoveredRadio] = []
    @Published private(set) var nodes: [UInt32: MeshNode] = [:]
    @Published private(set) var myNodeNum: UInt32?
    @Published private(set) var packetsReceived = 0
    @Published private(set) var textMessagesReceived = 0
    @Published private(set) var lastPacketAt: Date?
    @Published private(set) var log: [String] = []

    /// Called for every decrypted text packet.
    var onTextMessage: (@MainActor (MeshTextMessage) -> Void)?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var toRadio: CBCharacteristic?
    private var fromRadio: CBCharacteristic?
    private var fromNum: CBCharacteristic?
    private var draining = false
    private var drainRequested = false
    private var wantConfigID: UInt32 = 0
    private var heartbeatTimer: Timer?
    private var reconnectTask: Task<Void, Never>?

    private(set) var savedPeripheralID: UUID? {
        get { UserDefaults.standard.string(forKey: Self.savedPeripheralKey).flatMap { UUID(uuidString: $0) } }
        set { UserDefaults.standard.set(newValue?.uuidString, forKey: Self.savedPeripheralKey) }
    }

    override init() {
        super.init()
        // queue: nil → delegate callbacks on the main queue.
        central = CBCentralManager(delegate: self, queue: nil, options: [
            CBCentralManagerOptionRestoreIdentifierKey: Self.restoreIdentifier,
            CBCentralManagerOptionShowPowerAlertKey: true,
        ])
    }

    // MARK: - Public controls

    func startScan() {
        guard central.state == .poweredOn else { return }
        discovered.removeAll()
        state = .scanning
        // Meshtastic advertises its service UUID, which is also the only
        // filter iOS permits for scanning in the background.
        central.scanForPeripherals(withServices: [Self.serviceUUID],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
        append(log: "Scanning for Meshtastic radios")
    }

    func stopScan() {
        if central.isScanning { central.stopScan() }
        if state == .scanning { state = .idle }
    }

    func connect(to id: UUID) {
        guard let p = central.retrievePeripherals(withIdentifiers: [id]).first else {
            append(log: "Peripheral \(id.uuidString) is no longer available")
            return
        }
        stopScan()
        savedPeripheralID = id
        connect(peripheral: p)
    }

    /// Drop the radio and forget it; no automatic reconnects afterwards.
    func forget() {
        savedPeripheralID = nil
        reconnectTask?.cancel()
        if let p = peripheral {
            central.cancelPeripheralConnection(p)
        }
        clearLink()
        state = .idle
        append(log: "Forgot radio")
    }

    /// Meshtastic node whose short or long name matches `name`
    /// (case-insensitive), mirroring `_fetch_meshtastic_position` name matching.
    func node(named name: String) -> MeshNode? {
        let wanted = name.lowercased()
        return nodes.values.first { n in
            n.shortName.lowercased() == wanted || n.longName.lowercased() == wanted
        }
    }

    /// Resolve a detection node position: prefer the firmware `NODE_ID` name,
    /// then fall back to the Meshtastic node that relayed the packet.
    func position(forNodeID nodeID: String?, senderNum: UInt32?) -> (lat: Double, lon: Double)? {
        if let nodeID, let n = node(named: nodeID), let c = n.coordinate {
            return (c.latitude, c.longitude)
        }
        if let senderNum, let n = nodes[senderNum], let c = n.coordinate {
            return (c.latitude, c.longitude)
        }
        return nil
    }

    func name(ofNode num: UInt32) -> String? {
        nodes[num]?.displayName
    }

    // MARK: - Connection lifecycle

    private func connect(peripheral p: CBPeripheral) {
        peripheral = p
        p.delegate = self
        state = .connecting(p.name ?? "radio")
        append(log: "Connecting to \(p.name ?? p.identifier.uuidString)")
        // A pending connect never times out and survives backgrounding, which
        // is what re-joins the radio when it comes back into range.
        central.connect(p, options: [
            CBConnectPeripheralOptionNotifyOnConnectionKey: true,
            CBConnectPeripheralOptionNotifyOnDisconnectionKey: true,
            CBConnectPeripheralOptionNotifyOnNotificationKey: true,
        ])
    }

    private func reconnectSavedIfPossible() {
        guard central.state == .poweredOn, let id = savedPeripheralID else { return }
        if let p = peripheral, p.identifier == id, p.state == .connected || p.state == .connecting {
            return
        }
        if let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            connect(peripheral: p)
        }
    }

    private func scheduleReconnect(after seconds: Double) {
        reconnectTask?.cancel()
        reconnectTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
            guard !Task.isCancelled else { return }
            self?.reconnectSavedIfPossible()
        }
    }

    private func clearLink() {
        toRadio = nil
        fromRadio = nil
        fromNum = nil
        draining = false
        drainRequested = false
        heartbeatTimer?.invalidate()
        heartbeatTimer = nil
        peripheral = nil
    }

    private func beginConfigHandshake() {
        guard let p = peripheral, let toRadio, fromRadio != nil else { return }
        wantConfigID = UInt32.random(in: 1...UInt32.max)
        nodes.removeAll()
        state = .configuring(p.name ?? "radio")
        p.writeValue(ToRadio.wantConfig(id: wantConfigID), for: toRadio, type: .withResponse)
        append(log: "Requested config (id \(wantConfigID))")
        requestDrain()
    }

    private func requestDrain() {
        guard let p = peripheral, let fromRadio else { return }
        if draining {
            drainRequested = true
            return
        }
        draining = true
        p.readValue(for: fromRadio)
    }

    private func sendHeartbeat() {
        guard let p = peripheral, let toRadio, state.isConnected else { return }
        p.writeValue(ToRadio.heartbeat(), for: toRadio, type: .withoutResponse)
    }

    private func startHeartbeat() {
        heartbeatTimer?.invalidate()
        heartbeatTimer = Timer.scheduledTimer(withTimeInterval: Self.heartbeatInterval, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sendHeartbeat() }
        }
    }

    // MARK: - FromRadio handling

    private func handle(fromRadio data: Data) {
        let msg: FromRadio
        do {
            msg = try FromRadio.decode(data)
        } catch {
            append(log: "Undecodable FromRadio (\(data.count) B): \(error)")
            return
        }
        switch msg {
        case .myInfo(let num):
            myNodeNum = num
            append(log: String(format: "My node: !%08x", num))
        case .nodeInfo(let info):
            upsert(nodeInfo: info)
        case .configComplete(let id):
            if id == wantConfigID || wantConfigID == 0 {
                state = .connected(peripheral?.name ?? "radio")
                append(log: "Config complete — \(nodes.count) nodes known")
                startHeartbeat()
            }
        case .packet(let packet):
            handle(packet: packet)
        case .other:
            break
        }
    }

    private func handle(packet: MeshPacket) {
        packetsReceived += 1
        lastPacketAt = Date()
        guard let decoded = packet.decoded else { return }   // encrypted for another channel
        switch decoded.portnum {
        case MeshtasticProto.portTextMessage:
            guard let text = String(data: decoded.payload, encoding: .utf8) else { return }
            textMessagesReceived += 1
            let when = packet.rxTime > 0 ? Date(timeIntervalSince1970: TimeInterval(packet.rxTime)) : Date()
            let msg = MeshTextMessage(text: text, from: packet.from, to: packet.to,
                                      channel: packet.channel, rxTime: when,
                                      rxSNR: packet.rxSNR, rxRSSI: packet.rxRSSI)
            append(log: "\(name(ofNode: packet.from) ?? String(format: "!%08x", packet.from)): \(text)")
            onTextMessage?(msg)
        case MeshtasticProto.portPosition:
            if let pos = try? MeshPosition(decoded.payload) {
                var n = nodes[packet.from] ?? MeshNode(num: packet.from)
                apply(position: pos, to: &n)
                n.lastHeard = Date()
                nodes[packet.from] = n
            }
        case MeshtasticProto.portNodeInfo:
            if let user = try? MeshUser(decoded.payload) {
                var n = nodes[packet.from] ?? MeshNode(num: packet.from)
                n.shortName = user.shortName
                n.longName = user.longName
                n.lastHeard = Date()
                nodes[packet.from] = n
            }
        default:
            break
        }
    }

    private func upsert(nodeInfo info: MeshNodeInfo) {
        var n = nodes[info.num] ?? MeshNode(num: info.num)
        if let user = info.user {
            n.shortName = user.shortName
            n.longName = user.longName
        }
        if let pos = info.position {
            apply(position: pos, to: &n)
        }
        if info.lastHeard > 0 {
            n.lastHeard = Date(timeIntervalSince1970: TimeInterval(info.lastHeard))
        }
        nodes[info.num] = n
    }

    private func apply(position: MeshPosition, to node: inout MeshNode) {
        if let lat = position.latitude, let lon = position.longitude, lat != 0, lon != 0 {
            node.lat = lat
            node.lon = lon
            if let alt = position.altitude { node.altitude = Double(alt) }
        }
    }

    private func append(log line: String) {
        let stamp = DateFormatter.logTime.string(from: Date())
        log.append("\(stamp) \(line)")
        if log.count > Self.logLimit {
            log.removeFirst(log.count - Self.logLimit)
        }
    }

    // MARK: - Delegate bodies (main actor)

    private func centralStateChanged() {
        switch central.state {
        case .poweredOn:
            if state == .bluetoothOff || state == .unauthorized { state = .idle }
            append(log: "Bluetooth powered on")
            reconnectSavedIfPossible()
        case .poweredOff:
            state = .bluetoothOff
            clearLink()
        case .unauthorized:
            state = .unauthorized
        default:
            break
        }
    }

    private func restore(peripherals: [CBPeripheral]) {
        // iOS relaunched us because of a BLE event on a peripheral we were
        // connected to (or had a pending connect for). Re-adopt it.
        guard let p = peripherals.first else { return }
        peripheral = p
        p.delegate = self
        append(log: "Restored session with \(p.name ?? p.identifier.uuidString)")
        if p.state == .connected {
            state = .configuring(p.name ?? "radio")
            p.discoverServices([Self.serviceUUID])
        } else {
            state = .connecting(p.name ?? "radio")
        }
    }

    private func noteDiscovered(_ peripheral: CBPeripheral, localName: String?, rssi: Int) {
        let name = peripheral.name ?? localName ?? "Meshtastic"
        let entry = DiscoveredRadio(id: peripheral.identifier, name: name, rssi: rssi)
        if let idx = discovered.firstIndex(where: { $0.id == entry.id }) {
            discovered[idx] = entry
        } else {
            discovered.append(entry)
            discovered.sort { $0.rssi > $1.rssi }
        }
    }

    private func connected(_ peripheral: CBPeripheral) {
        reconnectTask?.cancel()
        append(log: "Connected to \(peripheral.name ?? peripheral.identifier.uuidString)")
        state = .configuring(peripheral.name ?? "radio")
        peripheral.discoverServices([Self.serviceUUID])
    }

    private func connectFailed(_ peripheral: CBPeripheral, error: Error?) {
        append(log: "Connect failed: \(error?.localizedDescription ?? "unknown")")
        clearLink()
        state = .idle
        scheduleReconnect(after: 5)
    }

    private func disconnected(_ peripheral: CBPeripheral, error: Error?) {
        append(log: "Disconnected: \(error?.localizedDescription ?? "by request")")
        clearLink()
        state = .idle
        if savedPeripheralID == peripheral.identifier {
            // Re-issue immediately; the pending connect waits for the radio
            // to reappear, even while the app is in the background.
            connect(peripheral: peripheral)
        }
    }

    private func servicesDiscovered(_ peripheral: CBPeripheral, error: Error?) {
        if let error {
            append(log: "Service discovery failed: \(error.localizedDescription)")
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else {
            append(log: "Meshtastic service not found on \(peripheral.name ?? "radio")")
            return
        }
        peripheral.discoverCharacteristics([Self.toRadioUUID, Self.fromRadioUUID, Self.fromNumUUID], for: service)
    }

    private func characteristicsDiscovered(_ peripheral: CBPeripheral, service: CBService, error: Error?) {
        if let error {
            append(log: "Characteristic discovery failed: \(error.localizedDescription)")
            return
        }
        for c in service.characteristics ?? [] {
            switch c.uuid {
            case Self.toRadioUUID:   toRadio = c
            case Self.fromRadioUUID: fromRadio = c
            case Self.fromNumUUID:   fromNum = c
            default: break
            }
        }
        guard toRadio != nil, fromRadio != nil, let fromNum else {
            append(log: "Radio is missing a required characteristic")
            return
        }
        peripheral.setNotifyValue(true, for: fromNum)
        beginConfigHandshake()
    }

    private func valueUpdated(_ peripheral: CBPeripheral, characteristic: CBCharacteristic, error: Error?) {
        if let error {
            append(log: "Read error on \(characteristic.uuid): \(error.localizedDescription)")
            if characteristic.uuid == Self.fromRadioUUID { draining = false }
            return
        }
        switch characteristic.uuid {
        case Self.fromNumUUID:
            // New packets are waiting on the radio.
            requestDrain()
        case Self.fromRadioUUID:
            if let data = characteristic.value, !data.isEmpty {
                handle(fromRadio: data)
                peripheral.readValue(for: characteristic)   // keep draining
            } else {
                draining = false
                if drainRequested {
                    drainRequested = false
                    requestDrain()
                }
            }
        default:
            break
        }
    }

    private func servicesModified(_ peripheral: CBPeripheral, invalidated: [CBService]) {
        // Firmware update or reboot re-published services; rediscover.
        if invalidated.contains(where: { $0.uuid == Self.serviceUUID }) {
            peripheral.discoverServices([Self.serviceUUID])
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension MeshtasticRadio: CBCentralManagerDelegate {

    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        MainActor.assumeIsolated { centralStateChanged() }
    }

    nonisolated func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        let peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] ?? []
        MainActor.assumeIsolated { restore(peripherals: peripherals) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                                    advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let localName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        MainActor.assumeIsolated { noteDiscovered(peripheral, localName: localName, rssi: RSSI.intValue) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        MainActor.assumeIsolated { connected(peripheral) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { connectFailed(peripheral, error: error) }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated { disconnected(peripheral, error: error) }
    }
}

// MARK: - CBPeripheralDelegate

extension MeshtasticRadio: CBPeripheralDelegate {

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        MainActor.assumeIsolated { servicesDiscovered(peripheral, error: error) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        MainActor.assumeIsolated { characteristicsDiscovered(peripheral, service: service, error: error) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated { valueUpdated(peripheral, characteristic: characteristic, error: error) }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let error else { return }
        MainActor.assumeIsolated { append(log: "Write to \(characteristic.uuid) failed: \(error.localizedDescription)") }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard let error else { return }
        MainActor.assumeIsolated { append(log: "Notify setup failed: \(error.localizedDescription)") }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        MainActor.assumeIsolated { servicesModified(peripheral, invalidated: invalidatedServices) }
    }
}

extension DateFormatter {
    static let logTime: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()
}
