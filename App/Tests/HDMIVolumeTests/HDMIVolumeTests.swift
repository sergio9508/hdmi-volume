import Foundation
import Testing
@testable import HDMIVolumeCore

@Suite("HDMIVolumeCore")
struct HDMIVolumeTests {
    @Test("MenuBarState defaults to visible and toggles persistence")
    func menuBarStateToggle() {
        let suiteName = "com.hdmivolume.tests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suiteName)!
        defer { defaults.removePersistentDomain(forName: suiteName) }

        let initial = MenuBarState(defaults: defaults)
        #expect(initial.isVisible == true)

        initial.hide()
        #expect(initial.isVisible == false)

        let reloaded = MenuBarState(defaults: defaults)
        #expect(reloaded.isVisible == false)

        reloaded.show()
        #expect(reloaded.isVisible == true)
    }

    @Test("Target selection skips virtual device and prefers HDMI/DisplayPort")
    func targetSelection() {
        let virtualDev = AudioOutputDevice(
            id: 1,
            uid: AudioDeviceManager.virtualDeviceUID,
            name: "HDMI Volume",
            transportType: 0x76697274 // 'virt'
        )
        let speakers = AudioOutputDevice(
            id: 2,
            uid: "BuiltInSpeakerDevice",
            name: "MacBook Air Speakers",
            transportType: 0x626c746e // 'bltn'
        )
        let dell = AudioOutputDevice(
            id: 3,
            uid: "DELLU3417W",
            name: "DELL U3417W",
            transportType: 0x68646d69 // 'hdmi'
        )

        let picked = AudioDeviceManager.selectPreferredTarget(from: [virtualDev, speakers, dell])
        #expect(picked?.uid == "DELLU3417W")

        let explicit = AudioDeviceManager.selectPreferredTarget(from: [virtualDev, speakers, dell], preferredUID: "BuiltInSpeakerDevice")
        #expect(explicit?.uid == "BuiltInSpeakerDevice")
    }

    @Test("FloatRingBuffer writes, reads, and zero-fills underruns")
    func ringBufferRoundTrip() {
        let ring = FloatRingBuffer(capacityFrames: 8, channels: 2)
        let input: [Float] = [0.5, -0.5, 0.25, -0.25]
        input.withUnsafeBufferPointer { ptr in
            ring.write(ptr.baseAddress!, count: 4)
        }

        var output = [Float](repeating: 9.0, count: 6)
        output.withUnsafeMutableBufferPointer { ptr in
            ring.read(into: ptr.baseAddress!, count: 6)
        }

        #expect(output[0] == 0.5)
        #expect(output[1] == -0.5)
        #expect(output[2] == 0.25)
        #expect(output[3] == -0.25)
        #expect(output[4] == 0.0)
        #expect(output[5] == 0.0)
    }

    @Test("FloatRingBuffer caps standing latency to maxLatencyFrames")
    func ringBufferLatencyCap() {
        let ring = FloatRingBuffer(capacityFrames: 16, maxLatencyFrames: 2, channels: 2)
        let chunk1: [Float] = [1, 1, 2, 2]
        let chunk2: [Float] = [3, 3, 4, 4]
        chunk1.withUnsafeBufferPointer { ring.write($0.baseAddress!, count: 4) }
        chunk2.withUnsafeBufferPointer { ring.write($0.baseAddress!, count: 4) }

        var output = [Float](repeating: -1, count: 4)
        output.withUnsafeMutableBufferPointer { ring.read(into: $0.baseAddress!, count: 4) }
        #expect(output == [3, 3, 4, 4])
    }
}
