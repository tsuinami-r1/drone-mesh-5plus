import Foundation

/// Port of `_rssi_raw_to_dbm` / `_rssi_to_max_range_m` from mesh-mapper.py:
/// turn an RX5808 12-bit ADC reading into an estimated maximum detection
/// radius using free-space path loss at 5.8 GHz.
enum RangeEstimator {
    static let fspl5800dB = 47.72
    /// Assumed FPV VTX power: 100 mW = 20 dBm.
    static let defaultTxDBm = 20.0

    static func rssiRawToDBm(_ rssiRaw: Int) -> Double {
        // Empirical: threshold 1800 ≈ -85 dBm, full-scale 4095 ≈ -15 dBm.
        let v = Double(max(0, min(4095, rssiRaw)))
        return -85.0 + (v - 1800) / (4095 - 1800) * 70.0
    }

    static func maxRangeMeters(rssiRaw: Int, txDBm: Double = defaultTxDBm) -> Double {
        max(1.0, pow(10.0, (txDBm - rssiRawToDBm(rssiRaw) - fspl5800dB) / 20.0))
    }
}
