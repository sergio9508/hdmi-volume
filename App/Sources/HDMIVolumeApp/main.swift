import AppKit
import CoreAudio
import Foundation
import HDMIVolumeCore

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let menuBarState = MenuBarState()
    private let bridge = AudioBridge()
    private let preferredTargetKey = "com.hdmivolume.app.preferredTargetUID"

    private var statusItem: NSStatusItem?
    private var previousDefaultDeviceID: AudioDeviceID?
    private var currentTarget: AudioOutputDevice?

    func applicationDidFinishLaunching(_ notification: Notification) {
        if CommandLine.arguments.contains("--show-icon") {
            menuBarState.show()
        }

        // If another instance is already running, ask it to show its menu bar icon and exit.
        let running = NSRunningApplication.runningApplications(withBundleIdentifier: Bundle.main.bundleIdentifier ?? "com.hdmivolume.app")
        if running.count > 1 {
            DistributedNotificationCenter.default().postNotificationName(
                MenuBarState.showIconNotification,
                object: nil,
                userInfo: nil,
                deliverImmediately: true
            )
            NSApp.terminate(nil)
            return
        }

        DistributedNotificationCenter.default().addObserver(
            self,
            selector: #selector(handleShowIconNotification),
            name: MenuBarState.showIconNotification,
            object: nil
        )

        if menuBarState.isVisible {
            createStatusItem()
        }

        startBridgeIfPossible()
        installDeviceChangeListener()
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showMenuBarIcon()
        return false
    }

    func applicationWillTerminate(_ notification: Notification) {
        bridge.stop()
        if let prev = previousDefaultDeviceID {
            AudioDeviceManager.setDefaultOutputDevice(prev)
        }
    }

    @objc private func handleShowIconNotification() {
        showMenuBarIcon()
    }

    private func createStatusItem() {
        guard statusItem == nil else { return }
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let button = item.button {
            button.image = NSImage(systemSymbolName: "speaker.wave.2.fill", accessibilityDescription: "HDMI Volume")
        }
        statusItem = item
        rebuildMenu()
    }

    private func removeStatusItem() {
        if let item = statusItem {
            NSStatusBar.system.removeStatusItem(item)
            statusItem = nil
        }
    }

    private func showMenuBarIcon() {
        menuBarState.show()
        createStatusItem()
    }

    @objc private func hideMenuBarIcon() {
        menuBarState.hide()
        removeStatusItem()
    }

    @objc private func selectTargetDevice(_ sender: NSMenuItem) {
        guard let uid = sender.representedObject as? String else { return }
        UserDefaults.standard.set(uid, forKey: preferredTargetKey)
        startBridgeIfPossible()
        rebuildMenu()
    }

    @objc private func quitApp() {
        NSApp.terminate(nil)
    }

    private func startBridgeIfPossible() {
        guard let virtualDev = AudioDeviceManager.findVirtualDevice() else {
            currentTarget = nil
            rebuildMenu()
            return
        }

        let devices = AudioDeviceManager.listPhysicalOutputDevices()
        let preferredUID = UserDefaults.standard.string(forKey: preferredTargetKey)
        guard let target = AudioDeviceManager.selectPreferredTarget(from: devices, preferredUID: preferredUID) else {
            bridge.stop()
            currentTarget = nil
            rebuildMenu()
            return
        }

        if previousDefaultDeviceID == nil {
            let currentDefault = AudioDeviceManager.defaultOutputDeviceID()
            if currentDefault != virtualDev.id {
                previousDefaultDeviceID = currentDefault
            }
        }

        if bridge.start(sourceDeviceID: virtualDev.id, targetDeviceID: target.id) {
            currentTarget = target
            AudioDeviceManager.setDefaultOutputDevice(virtualDev.id)
        }
        rebuildMenu()
    }

    private func rebuildMenu() {
        guard let statusItem else { return }
        let menu = NSMenu()

        let statusTitle: String
        if AudioDeviceManager.findVirtualDevice() == nil {
            statusTitle = "Driver not installed (run install.sh)"
        } else if let target = currentTarget {
            statusTitle = "Active → \(target.name)"
        } else {
            statusTitle = "No output device found"
        }
        let statusEntry = NSMenuItem(title: statusTitle, action: nil, keyEquivalent: "")
        statusEntry.isEnabled = false
        menu.addItem(statusEntry)
        menu.addItem(.separator())

        let targetMenuItem = NSMenuItem(title: "Output Target", action: nil, keyEquivalent: "")
        let targetSubmenu = NSMenu()
        for dev in AudioDeviceManager.listPhysicalOutputDevices() {
            let item = NSMenuItem(title: dev.name, action: #selector(selectTargetDevice(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = dev.uid
            item.state = (dev.uid == currentTarget?.uid) ? .on : .off
            targetSubmenu.addItem(item)
        }
        targetMenuItem.submenu = targetSubmenu
        menu.addItem(targetMenuItem)

        menu.addItem(.separator())

        let hideItem = NSMenuItem(title: "Hide Menu Bar Icon", action: #selector(hideMenuBarIcon), keyEquivalent: "h")
        hideItem.target = self
        menu.addItem(hideItem)

        let quitItem = NSMenuItem(title: "Quit HDMI Volume", action: #selector(quitApp), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)

        statusItem.menu = menu
    }

    private func installDeviceChangeListener() {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        AudioObjectAddPropertyListener(
            AudioObjectID(kAudioObjectSystemObject),
            &address,
            { _, _, _, clientData -> OSStatus in
                guard let clientData else { return noErr }
                let delegate = Unmanaged<AppDelegate>.fromOpaque(clientData).takeUnretainedValue()
                DispatchQueue.main.async {
                    delegate.startBridgeIfPossible()
                }
                return noErr
            },
            selfPtr
        )
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
