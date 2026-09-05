import SwiftUI
import MapKit

/// Live map: drone markers, pilot markers, flight paths, analog FM range
/// rings at the reporting node, and known Meshtastic nodes.
struct MapView: View {
    @EnvironmentObject private var store: DetectionStore
    @EnvironmentObject private var radio: MeshtasticRadio

    @State private var camera: MapCameraPosition = .userLocation(fallback: .automatic)
    @State private var showInactive = false
    @State private var showNodes = true
    @State private var selectedMAC: String?
    @State private var hasAutoFit = false

    private var shown: [Detection] {
        showInactive ? store.sortedDetections : store.activeDetections
    }

    var body: some View {
        NavigationStack {
            Map(position: $camera) {
                UserAnnotation()

                ForEach(shown) { det in
                    detectionContent(det)
                }

                if showNodes {
                    ForEach(nodePins) { pin in
                        Annotation(pin.name, coordinate: pin.coordinate, anchor: .center) {
                            Image(systemName: "dot.radiowaves.left.and.right")
                                .font(.caption)
                                .padding(4)
                                .background(.thinMaterial, in: Circle())
                        }
                    }
                }
            }
            .mapStyle(.standard(elevation: .flat, pointsOfInterest: .excludingAll))
            .mapControls {
                MapUserLocationButton()
                MapCompass()
                MapScaleView()
            }
            .overlay(alignment: .top) { statusBar }
            .navigationTitle("Mesh Mapper")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Menu {
                        Toggle("Show inactive", isOn: $showInactive)
                        Toggle("Show mesh nodes", isOn: $showNodes)
                    } label: {
                        Image(systemName: "line.3.horizontal.decrease.circle")
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        fitAll()
                    } label: {
                        Image(systemName: "arrow.up.left.and.arrow.down.right")
                    }
                    .disabled(allCoordinates.isEmpty)
                }
            }
            .sheet(item: selectedDetection) { det in
                DetectionDetailView(mac: det.mac)
                    .presentationDetents([.medium, .large])
            }
            .onChange(of: store.activeDetections.count) { _, count in
                if !hasAutoFit, count > 0 {
                    hasAutoFit = true
                    fitAll()
                }
            }
        }
    }

    // MARK: Map content

    @MapContentBuilder
    private func detectionContent(_ det: Detection) -> some MapContent {
        let name = det.displayName(aliases: store.aliases)
        let dim = det.status != .active

        if det.track.count > 1 {
            MapPolyline(coordinates: det.track.map { CLLocationCoordinate2D(latitude: $0.lat, longitude: $0.lon) })
                .stroke(det.kind.color.opacity(dim ? 0.3 : 0.8), lineWidth: 3)
        }

        if let c = det.droneCoordinate {
            Annotation(name, coordinate: c, anchor: .center) {
                Button { selectedMAC = det.mac } label: {
                    Image(systemName: det.kind.symbol)
                        .font(.title2)
                        .foregroundStyle(det.kind.color)
                        .padding(6)
                        .background(.regularMaterial, in: Circle())
                        .opacity(dim ? 0.45 : 1)
                }
                .buttonStyle(.plain)
            }
        }

        if let p = det.pilotCoordinate {
            Annotation("Pilot · \(name)", coordinate: p, anchor: .center) {
                Button { selectedMAC = det.mac } label: {
                    Image(systemName: "person.fill")
                        .font(.title3)
                        .foregroundStyle(.blue)
                        .padding(6)
                        .background(.regularMaterial, in: Circle())
                        .opacity(dim ? 0.45 : 1)
                }
                .buttonStyle(.plain)
            }
        }

        if let dc = det.droneCoordinate, let pc = det.pilotCoordinate {
            MapPolyline(coordinates: [dc, pc])
                .stroke(.blue.opacity(dim ? 0.2 : 0.5), style: StrokeStyle(lineWidth: 1.5, dash: [4, 4]))
        }

        if det.kind == .analogFM, let n = det.nodeCoordinate {
            if let r = det.radiusM, r > 0 {
                MapCircle(center: n, radius: r)
                    .foregroundStyle(.purple.opacity(dim ? 0.05 : 0.12))
                    .stroke(.purple.opacity(dim ? 0.3 : 0.9), lineWidth: 2)
            }
            Annotation(name, coordinate: n, anchor: .center) {
                Button { selectedMAC = det.mac } label: {
                    Text("📡")
                        .font(.title2)
                        .padding(4)
                        .background(.regularMaterial, in: Circle())
                        .opacity(dim ? 0.45 : 1)
                }
                .buttonStyle(.plain)
            }
        }
    }

    // MARK: Status bar

    private var statusBar: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(radio.state.isConnected ? Color.green : Color.orange)
                .frame(width: 8, height: 8)
            Text(radio.state.label)
                .font(.caption)
                .lineLimit(1)
            Spacer()
            Text("\(store.activeDetections.count) active")
                .font(.caption.monospacedDigit())
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(.thinMaterial, in: Capsule())
        .padding(.horizontal)
        .padding(.top, 4)
    }

    // MARK: Helpers

    private struct NodePin: Identifiable {
        let id: UInt32
        let name: String
        let coordinate: CLLocationCoordinate2D
    }

    private var nodePins: [NodePin] {
        radio.nodes.values.compactMap { node in
            node.coordinate.map { NodePin(id: node.num, name: node.displayName, coordinate: $0) }
        }
    }

    private var selectedDetection: Binding<Detection?> {
        Binding(
            get: { selectedMAC.flatMap { store.detections[$0] } },
            set: { selectedMAC = $0?.mac }
        )
    }

    private var allCoordinates: [CLLocationCoordinate2D] {
        var out: [CLLocationCoordinate2D] = []
        for det in shown {
            if let c = det.droneCoordinate { out.append(c) }
            if let p = det.pilotCoordinate { out.append(p) }
            if let n = det.nodeCoordinate { out.append(n) }
        }
        return out
    }

    private func fitAll() {
        let coords = allCoordinates
        guard !coords.isEmpty else { return }
        var minLat = coords[0].latitude, maxLat = coords[0].latitude
        var minLon = coords[0].longitude, maxLon = coords[0].longitude
        for c in coords {
            minLat = min(minLat, c.latitude); maxLat = max(maxLat, c.latitude)
            minLon = min(minLon, c.longitude); maxLon = max(maxLon, c.longitude)
        }
        let center = CLLocationCoordinate2D(latitude: (minLat + maxLat) / 2, longitude: (minLon + maxLon) / 2)
        let span = MKCoordinateSpan(latitudeDelta: max(0.01, (maxLat - minLat) * 1.6),
                                    longitudeDelta: max(0.01, (maxLon - minLon) * 1.6))
        withAnimation {
            camera = .region(MKCoordinateRegion(center: center, span: span))
        }
    }
}
