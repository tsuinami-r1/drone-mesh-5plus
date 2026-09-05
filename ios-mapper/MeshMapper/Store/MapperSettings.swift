import Foundation

/// User-tunable behaviour, persisted in UserDefaults.
struct MapperSettings: Codable, Equatable {
    /// Same role as `staleThreshold` in mesh-mapper.py (seconds). Detections
    /// go `inactive` after 3× this and `inactive_old` after 30× this.
    var staleThresholdSeconds: Double = 60

    /// Assumed FPV transmitter power for analog range rings (dBm).
    var assumedVTXPowerDBm: Double = RangeEstimator.defaultTxDBm

    var alertOnGPSDrones = true
    var alertOnNoGPSDrones = true
    var alertOnDJI = true
    var alertOnAnalogFM = true

    /// Replay canned mesh alerts so the UI can be exercised in the simulator
    /// with no radio attached.
    var demoFeedEnabled = false

    private static let key = "mapper.settings"

    static func load() -> MapperSettings {
        guard let data = UserDefaults.standard.data(forKey: key),
              let s = try? JSONDecoder().decode(MapperSettings.self, from: data) else {
            return MapperSettings()
        }
        return s
    }

    func save() {
        if let data = try? JSONEncoder().encode(self) {
            UserDefaults.standard.set(data, forKey: Self.key)
        }
    }
}
