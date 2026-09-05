import SwiftUI
import UserNotifications
import Combine

/// Owns the long-lived objects and wires them together. Created by the app
/// delegate at launch so the Bluetooth central (with its restore identifier)
/// exists before `application(_:didFinishLaunchingWithOptions:)` returns —
/// a requirement for CoreBluetooth state restoration to deliver
/// `willRestoreState` after iOS relaunches the app in the background.
@MainActor
final class AppModel {
    let notifier = AlertNotifier()
    let radio = MeshtasticRadio()
    let location = LocationProvider()
    let store: DetectionStore
    private var demo: DemoFeed?
    private var cancellables = Set<AnyCancellable>()

    init() {
        store = DetectionStore(notifier: notifier)

        radio.onTextMessage = { [store] message in
            store.ingest(message: message)
        }
        store.senderNameResolver = { [radio] num in
            radio.name(ofNode: num)
        }
        store.nodePositionResolver = { [radio, store] nodeID, sender in
            if let pos = radio.position(forNodeID: nodeID, senderNum: sender) {
                return pos
            }
            return store.settings.demoFeedEnabled ? DemoFeed.nodePosition(forNodeID: nodeID) : nil
        }

        store.$settings
            .map(\.demoFeedEnabled)
            .removeDuplicates()
            .receive(on: DispatchQueue.main)
            .sink { [weak self] enabled in
                guard let self else { return }
                if enabled {
                    if self.demo == nil { self.demo = DemoFeed(store: self.store) }
                    self.demo?.start()
                } else {
                    self.demo?.stop()
                }
            }
            .store(in: &cancellables)
    }
}

@MainActor
final class AppDelegate: NSObject, UIApplicationDelegate, UNUserNotificationCenterDelegate {
    let model = AppModel()

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
        UNUserNotificationCenter.current().delegate = self
        model.notifier.requestAuthorization()
        return true
    }

    /// Show banners even while the app is in the foreground.
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter,
                                            willPresent notification: UNNotification,
                                            withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .list, .sound])
    }
}

@main
struct MeshMapperApp: App {
    @UIApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(appDelegate.model.store)
                .environmentObject(appDelegate.model.radio)
                .environmentObject(appDelegate.model.location)
                .onAppear { appDelegate.model.location.start() }
        }
        .onChange(of: scenePhase) { _, phase in
            if phase == .background {
                appDelegate.model.store.persistIfNeeded()
            }
        }
    }
}
