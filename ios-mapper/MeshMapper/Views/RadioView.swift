import SwiftUI

/// Pair with a Meshtastic radio over BLE and inspect the link.
struct RadioView: View {
    @EnvironmentObject private var radio: MeshtasticRadio

    var body: some View {
        NavigationStack {
            List {
                Section("Link") {
                    HStack {
                        Circle()
                            .fill(radio.state.isConnected ? Color.green : Color.orange)
                            .frame(width: 10, height: 10)
                        Text(radio.state.label)
                    }
                    if let my = radio.myNodeNum {
                        LabeledContent("This radio", value: String(format: "!%08x", my))
                    }
                    LabeledContent("Packets", value: "\(radio.packetsReceived)")
                    LabeledContent("Text messages", value: "\(radio.textMessagesReceived)")
                    if let t = radio.lastPacketAt {
                        LabeledContent("Last packet", value: Format.age(since: t))
                    }
                    if radio.savedPeripheralID != nil {
                        Button("Forget radio", role: .destructive) { radio.forget() }
                    }
                }

                Section {
                    if radio.state == .scanning {
                        Button("Stop scanning") { radio.stopScan() }
                    } else {
                        Button("Scan for radios") { radio.startScan() }
                            .disabled(radio.state == .bluetoothOff || radio.state == .unauthorized)
                    }
                    ForEach(radio.discovered) { r in
                        Button {
                            radio.connect(to: r.id)
                        } label: {
                            HStack {
                                Text(r.name)
                                Spacer()
                                Text("\(r.rssi) dBm")
                                    .font(.caption.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                } header: {
                    Text("Radios")
                } footer: {
                    Text("Pair the radio in the Meshtastic app first if it asks for a PIN, then disconnect it there — a Meshtastic radio accepts one Bluetooth client at a time.")
                }

                Section("Mesh nodes (\(radio.nodes.count))") {
                    ForEach(radio.nodes.values.sorted { ($0.lastHeard ?? .distantPast) > ($1.lastHeard ?? .distantPast) }) { node in
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                Text(node.displayName).font(.headline)
                                if !node.shortName.isEmpty {
                                    Text(node.shortName).font(.caption).foregroundStyle(.secondary)
                                }
                                Spacer()
                                if let t = node.lastHeard {
                                    Text(Format.age(since: t)).font(.caption2).foregroundStyle(.secondary)
                                }
                            }
                            if let c = node.coordinate {
                                Text(Format.coordinate(c.latitude, c.longitude))
                                    .font(.caption.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            } else {
                                Text("no position").font(.caption).foregroundStyle(.tertiary)
                            }
                        }
                    }
                }

                Section("Log") {
                    ForEach(Array(radio.log.suffix(60).reversed().enumerated()), id: \.offset) { item in
                        Text(item.element)
                            .font(.caption.monospaced())
                            .lineLimit(3)
                    }
                }
            }
            .navigationTitle("Radio")
        }
    }
}
