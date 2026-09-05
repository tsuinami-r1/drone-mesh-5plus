import Foundation
import UserNotifications

/// Posts local notifications for detection events. This is the phone-side
/// stand-in for the mapper's webhook / popup: it is what makes a background
/// detection visible on the lock screen.
final class AlertNotifier {

    private let center = UNUserNotificationCenter.current()
    private(set) var authorized = false

    func requestAuthorization() {
        center.requestAuthorization(options: [.alert, .sound, .badge]) { [weak self] granted, _ in
            self?.authorized = granted
        }
    }

    /// Fire an immediate notification. `thread` groups repeat alerts for the
    /// same emitter; `identifier` replaces an earlier pending alert with the
    /// same id so a chatty drone does not stack dozens of banners.
    func post(title: String, body: String, thread: String, identifier: String? = nil) {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        content.threadIdentifier = thread
        if #available(iOS 15.0, *) {
            content.interruptionLevel = .timeSensitive
        }
        let request = UNNotificationRequest(identifier: identifier ?? UUID().uuidString,
                                            content: content, trigger: nil)
        center.add(request)
    }
}
