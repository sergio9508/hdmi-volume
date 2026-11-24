import CoreAudio
import Foundation

public struct AudioOutputDevice: Equatable, Sendable {
    public let id: AudioDeviceID
    public let uid: String
    public let name: String
    public let transportType: UInt32

    public init(id: AudioDeviceID, uid: String, name: String, transportType: UInt32) {
        self.id = id
        self.uid = uid
        self.name = name
        self.transportType = transportType
    }

    public var isExternalDisplayAudio: Bool {
        transportType == kAudioDeviceTransportTypeHDMI ||
        transportType == kAudioDeviceTransportTypeDisplayPort
    }
}

public enum AudioDeviceManager {
    public static let virtualDeviceUID = "com.hdmivolume.driver.device"

    public static func listOutputDevices() -> [AudioOutputDevice] {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size) == noErr,
              size > 0 else {
            return []
        }

        let count = Int(size) / MemoryLayout<AudioDeviceID>.size
        var deviceIDs = [AudioDeviceID](repeating: 0, count: count)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceIDs) == noErr else {
            return []
        }

        return deviceIDs.compactMap { id in
            guard hasOutputChannels(deviceID: id),
                  let uid = stringProperty(deviceID: id, selector: kAudioDevicePropertyDeviceUID),
                  let name = stringProperty(deviceID: id, selector: kAudioObjectPropertyName) else {
                return nil
            }
            let transport = uint32Property(deviceID: id, selector: kAudioDevicePropertyTransportType) ?? 0
            return AudioOutputDevice(id: id, uid: uid, name: name, transportType: transport)
        }
    }

    public static func findVirtualDevice() -> AudioOutputDevice? {
        listOutputDevices().first { $0.uid == virtualDeviceUID }
    }

    public static func listPhysicalOutputDevices() -> [AudioOutputDevice] {
        listOutputDevices().filter { $0.uid != virtualDeviceUID }
    }

    /// Pure selection helper: prefers explicit user choice, then HDMI/DisplayPort, then first available.
    public static func selectPreferredTarget(
        from devices: [AudioOutputDevice],
        preferredUID: String? = nil
    ) -> AudioOutputDevice? {
        let candidates = devices.filter { $0.uid != virtualDeviceUID }
        if let preferredUID, let match = candidates.first(where: { $0.uid == preferredUID }) {
            return match
        }
        if let external = candidates.first(where: { $0.isExternalDisplayAudio }) {
            return external
        }
        return candidates.first
    }

    public static func defaultOutputDeviceID() -> AudioDeviceID? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var deviceID: AudioDeviceID = 0
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &deviceID) == noErr else {
            return nil
        }
        return deviceID
    }

    @discardableResult
    public static func setDefaultOutputDevice(_ deviceID: AudioDeviceID) -> Bool {
        var mutableID = deviceID
        let size = UInt32(MemoryLayout<AudioDeviceID>.size)
        var outAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let status = AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject), &outAddress, 0, nil, size, &mutableID)

        var sysAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultSystemOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectSetPropertyData(AudioObjectID(kAudioObjectSystemObject), &sysAddress, 0, nil, size, &mutableID)
        return status == noErr
    }

    private static func hasOutputChannels(deviceID: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamConfiguration,
            mScope: kAudioObjectPropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(deviceID, &address, 0, nil, &size) == noErr, size > 0 else {
            return false
        }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, raw) == noErr else {
            return false
        }
        let bufferList = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        return bufferList.reduce(0) { $0 + Int($1.mNumberChannels) } > 0
    }

    private static func stringProperty(deviceID: AudioDeviceID, selector: AudioObjectPropertySelector) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &value) == noErr,
              let cfString = value?.takeRetainedValue() else {
            return nil
        }
        return cfString as String
    }

    private static func uint32Property(deviceID: AudioDeviceID, selector: AudioObjectPropertySelector) -> UInt32? {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(deviceID, &address, 0, nil, &size, &value) == noErr else {
            return nil
        }
        return value
    }
}
