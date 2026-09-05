import SwiftUI

struct SettingsView: View {
    @EnvironmentObject private var store: DetectionStore
    @State private var exportURL: URL?
    @State private var confirmClear = false

    private var staleMinutes: Binding<Double> {
        Binding(
            get: { store.settings.staleThresholdSeconds / 60 },
            set: { store.settings.staleThresholdSeconds = max(0.5, $0) * 60 }
        )
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle("Drones with GPS", isOn: $store.settings.alertOnGPSDrones)
                    Toggle("Drones without GPS", isOn: $store.settings.alertOnNoGPSDrones)
                    Toggle("DJI DroneID", isOn: $store.settings.alertOnDJI)
                    Toggle("Analog FPV video", isOn: $store.settings.alertOnAnalogFM)
                } header: {
                    Text("Notifications")
                } footer: {
                    Text("Alerts fire when an emitter transitions to active, and once per session for drones with no fix. They are delivered as time-sensitive notifications so they surface while the app is in the background.")
                }

                Section {
                    Stepper(value: staleMinutes, in: 0.5...30, step: 0.5) {
                        LabeledContent("Stale threshold", value: String(format: "%.1f min", staleMinutes.wrappedValue))
                    }
                    Text("Detections turn inactive after 3× this and old after 30× this. Analog FPV rings drop after 30 s of silence.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                } header: {
                    Text("Ageing")
                }

                Section {
                    Slider(value: $store.settings.assumedVTXPowerDBm, in: 10...30, step: 1) {
                        Text("VTX power")
                    } minimumValueLabel: {
                        Text("10")
                    } maximumValueLabel: {
                        Text("30")
                    }
                    LabeledContent("Assumed VTX power", value: "\(Int(store.settings.assumedVTXPowerDBm)) dBm")
                } header: {
                    Text("Analog range rings")
                } footer: {
                    Text("Ring radius is the free-space maximum range at 5.8 GHz for this transmitter power (20 dBm = 100 mW).")
                }

                Section {
                    Button("Export session CSV") {
                        exportURL = try? store.exportCSV()
                    }
                    .disabled(store.history.isEmpty)
                    if let url = exportURL {
                        ShareLink(item: url) { Label("Share \(url.lastPathComponent)", systemImage: "square.and.arrow.up") }
                    }
                    Button("Clear session", role: .destructive) { confirmClear = true }
                        .confirmationDialog("Clear all detections and history?", isPresented: $confirmClear) {
                            Button("Clear", role: .destructive) {
                                store.clearSession()
                                exportURL = nil
                            }
                        }
                } header: {
                    Text("Session")
                } footer: {
                    Text("\(store.history.count) logged rows · \(store.detections.count) tracked emitters · \(store.aliases.count) aliases")
                }

                Section {
                    Toggle("Demo feed (no radio needed)", isOn: $store.settings.demoFeedEnabled)
                } header: {
                    Text("Development")
                } footer: {
                    Text("Replays sample mesh alerts every few seconds so the map and notifications can be tested in the simulator.")
                }
            }
            .navigationTitle("Settings")
        }
    }
}
