import SwiftUI
import MapKit

struct DetectionListView: View {
    @EnvironmentObject private var store: DetectionStore
    @EnvironmentObject private var location: LocationProvider

    var body: some View {
        NavigationStack {
            List {
                if store.detections.isEmpty {
                    ContentUnavailableView("No detections yet",
                                           systemImage: "airplane",
                                           description: Text("Alerts from the mesh will appear here."))
                }
                if !store.activeDetections.isEmpty {
                    Section("Active") {
                        ForEach(store.activeDetections) { det in
                            NavigationLink(value: det.mac) { DetectionRow(det: det) }
                        }
                    }
                }
                if !store.inactiveDetections.isEmpty {
                    Section("Inactive") {
                        ForEach(store.inactiveDetections) { det in
                            NavigationLink(value: det.mac) { DetectionRow(det: det).opacity(0.6) }
                        }
                    }
                }
            }
            .navigationTitle("Detections")
            .navigationDestination(for: String.self) { mac in
                DetectionDetailView(mac: mac)
            }
        }
    }
}

struct DetectionRow: View {
    @EnvironmentObject private var store: DetectionStore
    @EnvironmentObject private var location: LocationProvider
    let det: Detection

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: det.kind.symbol)
                .foregroundStyle(det.kind.color)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(det.displayName(aliases: store.aliases))
                    .font(.headline)
                    .lineLimit(1)
                Text(subtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                if let rssi = det.rssi {
                    Text(det.kind == .analogFM ? "raw \(rssi)" : "\(rssi) dBm")
                        .font(.caption.monospacedDigit())
                }
                Text(Format.age(since: det.lastUpdate))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private var subtitle: String {
        var parts: [String] = [det.kind.label]
        if det.kind == .analogFM {
            if let node = det.nodeID { parts.append("node \(node)") }
            if let r = det.radiusM { parts.append("≤ \(Int(r)) m") }
        } else if let c = det.droneCoordinate {
            if let d = location.distance(to: c) { parts.append(Format.distance(d) + " away") }
            else { parts.append(Format.coordinate(c.latitude, c.longitude)) }
            if det.hasPilotGPS { parts.append("pilot") }
        } else {
            parts.append("no GPS")
        }
        return parts.joined(separator: " · ")
    }
}

struct DetectionDetailView: View {
    @EnvironmentObject private var store: DetectionStore
    @EnvironmentObject private var location: LocationProvider
    let mac: String
    @State private var aliasDraft = ""

    private var det: Detection? { store.detections[mac] }

    var body: some View {
        if let det {
            List {
                Section {
                    LabeledContent("Type", value: det.kind.label)
                    LabeledContent("Status", value: det.status.label)
                    LabeledContent("MAC", value: det.mac)
                    if let id = det.basicID, !id.isEmpty { LabeledContent("ID", value: id) }
                    if let band = det.band { LabeledContent("Band", value: band) }
                    if let rssi = det.rssi {
                        LabeledContent("RSSI", value: det.kind == .analogFM ? "\(rssi) (ADC)" : "\(rssi) dBm")
                    }
                    LabeledContent("First seen", value: det.firstSeen.formatted(date: .abbreviated, time: .standard))
                    LabeledContent("Last update", value: det.lastUpdate.formatted(date: .omitted, time: .standard))
                    if let sender = det.senderName { LabeledContent("Relayed by", value: sender) }
                }

                Section("Alias") {
                    HStack {
                        TextField("Alias for this emitter", text: $aliasDraft)
                            .textInputAutocapitalization(.never)
                        Button("Save") { store.setAlias(aliasDraft, for: mac) }
                            .disabled(aliasDraft == (store.aliases[mac] ?? ""))
                    }
                }

                if det.kind == .analogFM {
                    Section("Analog FPV") {
                        if let f = det.freqMHz { LabeledContent("Frequency", value: "\(f) MHz") }
                        if let b = det.band, let c = det.channel { LabeledContent("Channel", value: "\(b)\(c)") }
                        if let n = det.nodeID { LabeledContent("Node", value: n) }
                        if let r = det.radiusM { LabeledContent("Est. max range", value: "\(Int(r)) m") }
                        if let c = det.nodeCoordinate {
                            LabeledContent("Node position", value: Format.coordinate(c.latitude, c.longitude))
                            openInMapsButton(c, label: "Open node in Maps")
                        } else {
                            Text("Node position unknown — name the paired Meshtastic node \(det.nodeID ?? "NODE_ID") so it can be resolved.")
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        }
                    }
                } else {
                    Section("Drone") {
                        if let c = det.droneCoordinate {
                            LabeledContent("Position", value: Format.coordinate(c.latitude, c.longitude))
                            LabeledContent("Altitude", value: String(format: "%.0f m", det.droneAltitude))
                            if let d = location.distance(to: c) { LabeledContent("Distance", value: Format.distance(d)) }
                            LabeledContent("Track points", value: "\(det.track.count)")
                            openInMapsButton(c, label: "Open drone in Maps")
                        } else {
                            Text("No GPS fix received.")
                                .foregroundStyle(.secondary)
                        }
                    }
                    Section("Pilot") {
                        if let p = det.pilotCoordinate {
                            LabeledContent("Position", value: Format.coordinate(p.latitude, p.longitude))
                            if let d = location.distance(to: p) { LabeledContent("Distance", value: Format.distance(d)) }
                            openInMapsButton(p, label: "Open pilot in Maps")
                        } else {
                            Text("No operator position received.")
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .navigationTitle(det.displayName(aliases: store.aliases))
            .navigationBarTitleDisplayMode(.inline)
            .onAppear { aliasDraft = store.aliases[mac] ?? "" }
        } else {
            ContentUnavailableView("Detection removed", systemImage: "xmark.circle")
        }
    }

    private func openInMapsButton(_ c: CLLocationCoordinate2D, label: String) -> some View {
        Button(label) {
            let item = MKMapItem(placemark: MKPlacemark(coordinate: c))
            item.name = det?.displayName(aliases: store.aliases) ?? mac
            item.openInMaps()
        }
    }
}

struct AlertsView: View {
    @EnvironmentObject private var store: DetectionStore

    var body: some View {
        NavigationStack {
            List {
                if store.events.isEmpty {
                    ContentUnavailableView("No alerts yet", systemImage: "bell.slash",
                                           description: Text("Detection alerts are listed here and delivered as notifications, including while the app is in the background."))
                }
                ForEach(store.events) { e in
                    VStack(alignment: .leading, spacing: 2) {
                        HStack {
                            Text(e.title).font(.headline)
                            Spacer()
                            Text(e.time.formatted(date: .omitted, time: .shortened))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Text(e.body).font(.subheadline)
                    }
                }
            }
            .navigationTitle("Alerts")
        }
    }
}
