import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var store: DetectionStore
    @EnvironmentObject private var radio: MeshtasticRadio

    var body: some View {
        TabView {
            MapView()
                .tabItem { Label("Map", systemImage: "map") }
            DetectionListView()
                .tabItem { Label("Detections", systemImage: "list.bullet") }
                .badge(store.activeDetections.count)
            AlertsView()
                .tabItem { Label("Alerts", systemImage: "bell") }
            RadioView()
                .tabItem { Label("Radio", systemImage: radio.state.isConnected ? "antenna.radiowaves.left.and.right" : "antenna.radiowaves.left.and.right.slash") }
            SettingsView()
                .tabItem { Label("Settings", systemImage: "gear") }
        }
    }
}

// MARK: - Shared formatting helpers

enum Format {
    static func age(since date: Date, now: Date = Date()) -> String {
        let s = Int(now.timeIntervalSince(date))
        if s < 60 { return "\(s)s ago" }
        if s < 3600 { return "\(s / 60)m ago" }
        return "\(s / 3600)h \((s % 3600) / 60)m ago"
    }

    static func distance(_ meters: Double) -> String {
        meters < 1000 ? String(format: "%.0f m", meters) : String(format: "%.1f km", meters / 1000)
    }

    static func coordinate(_ lat: Double, _ lon: Double) -> String {
        String(format: "%.5f, %.5f", lat, lon)
    }
}

extension DetectionKind {
    var color: Color {
        switch self {
        case .remoteID: return .red
        case .dji:      return .orange
        case .analogFM: return .purple
        }
    }
}

extension DetectionStatus {
    var label: String {
        switch self {
        case .active:      return "Active"
        case .inactive:    return "Inactive"
        case .inactiveOld: return "Old"
        }
    }
}
