import Foundation

public final class MenuBarState: @unchecked Sendable {
    public static let defaultKey = "com.hdmivolume.app.showMenuBarIcon"
    public static let showIconNotification = Notification.Name("com.hdmivolume.app.showIcon")

    private let defaults: UserDefaults
    private let key: String

    public private(set) var isVisible: Bool

    public init(defaults: UserDefaults = .standard, key: String = MenuBarState.defaultKey) {
        self.defaults = defaults
        self.key = key
        if defaults.object(forKey: key) == nil {
            self.isVisible = true
        } else {
            self.isVisible = defaults.bool(forKey: key)
        }
    }

    public func hide() {
        isVisible = false
        defaults.set(false, forKey: key)
    }

    public func show() {
        isVisible = true
        defaults.set(true, forKey: key)
    }
}
