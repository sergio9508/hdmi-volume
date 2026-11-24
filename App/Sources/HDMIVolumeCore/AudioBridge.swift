import AudioToolbox
import CoreAudio
import Foundation

/// Bridges system audio via CoreAudio Process Tap (no microphone indicator) to a physical output device,
/// scaling output in real time from the virtual HDMI Volume device's volume and mute controls.
public final class AudioBridge: @unchecked Sendable {
    private var tapID: AudioObjectID = kAudioObjectUnknown
    private var aggregateDeviceID: AudioDeviceID = kAudioObjectUnknown
    private var tapIOProcID: AudioDeviceIOProcID?
    private var outputUnit: AudioUnit?

    private let ring = FloatRingBuffer(capacityFrames: 4096, maxLatencyFrames: 768, channels: 2)
    private var gainLock = os_unfair_lock_s()
    private var currentGain: Float = 0.75
    private var isRunning = false
    private var listenerDeviceID: AudioDeviceID = kAudioObjectUnknown

    public private(set) var sourceDeviceID: AudioDeviceID?
    public private(set) var targetDeviceID: AudioDeviceID?

    public init() {}

    deinit {
        stop()
    }

    public func start(sourceDeviceID: AudioDeviceID, targetDeviceID: AudioDeviceID) -> Bool {
        stop()
        ring.reset()

        refreshGain(from: sourceDeviceID)

        let excludeProcesses: [AudioObjectID]
        if let selfObj = AudioDeviceManager.currentProcessAudioObjectID() {
            excludeProcesses = [selfObj]
        } else {
            excludeProcesses = []
        }

        let tapDesc = CATapDescription(stereoGlobalTapButExcludeProcesses: excludeProcesses)
        let tapUUID = UUID()
        tapDesc.uuid = tapUUID
        tapDesc.name = "HDMIVolumeTap"
        tapDesc.isPrivate = true
        tapDesc.muteBehavior = .unmuted

        var createdTapID: AudioObjectID = kAudioObjectUnknown
        guard AudioHardwareCreateProcessTap(tapDesc, &createdTapID) == noErr,
              createdTapID != kAudioObjectUnknown else {
            return false
        }

        let aggDesc: [String: Any] = [
            kAudioAggregateDeviceNameKey: "HDMIVolumeTapDevice",
            kAudioAggregateDeviceUIDKey: "com.hdmivolume.tap.\(tapUUID.uuidString)",
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceTapListKey: [
                [
                    kAudioSubTapUIDKey: tapUUID.uuidString,
                    kAudioSubTapDriftCompensationKey: true
                ]
            ]
        ]

        var createdAggID: AudioDeviceID = kAudioObjectUnknown
        guard AudioHardwareCreateAggregateDevice(aggDesc as CFDictionary, &createdAggID) == noErr,
              createdAggID != kAudioObjectUnknown else {
            AudioHardwareDestroyProcessTap(createdTapID)
            return false
        }

        guard let outUnit = createOutputHALUnit(deviceID: targetDeviceID) else {
            AudioHardwareDestroyAggregateDevice(createdAggID)
            AudioHardwareDestroyProcessTap(createdTapID)
            return false
        }

        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        var outCallback = AURenderCallbackStruct(
            inputProc: outputRenderCallback,
            inputProcRefCon: selfPtr
        )
        guard AudioUnitSetProperty(
            outUnit,
            kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input,
            0,
            &outCallback,
            UInt32(MemoryLayout<AURenderCallbackStruct>.size)
        ) == noErr else {
            disposeUnit(outUnit)
            AudioHardwareDestroyAggregateDevice(createdAggID)
            AudioHardwareDestroyProcessTap(createdTapID)
            return false
        }

        var procID: AudioDeviceIOProcID?
        let ioProcStatus = AudioDeviceCreateIOProcIDWithBlock(&procID, createdAggID, nil) { [weak self] _, inInputData, _, _, _ in
            guard let self, self.isRunning else { return }
            let buffers = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: inInputData))
            guard let first = buffers.first, let data = first.mData else { return }
            let sampleCount = Int(first.mDataByteSize) / MemoryLayout<Float>.size
            guard sampleCount > 0 else { return }
            let floatPtr = data.assumingMemoryBound(to: Float.self)
            self.ring.write(floatPtr, count: sampleCount)
        }

        guard ioProcStatus == noErr, let procID else {
            disposeUnit(outUnit)
            AudioHardwareDestroyAggregateDevice(createdAggID)
            AudioHardwareDestroyProcessTap(createdTapID)
            return false
        }

        setPreferredBufferSize(deviceID: targetDeviceID, frames: 256)
        setPreferredBufferSize(deviceID: createdAggID, frames: 256)

        guard AudioUnitInitialize(outUnit) == noErr,
              AudioOutputUnitStart(outUnit) == noErr,
              AudioDeviceStart(createdAggID, procID) == noErr else {
            AudioDeviceDestroyIOProcID(createdAggID, procID)
            disposeUnit(outUnit)
            AudioHardwareDestroyAggregateDevice(createdAggID)
            AudioHardwareDestroyProcessTap(createdTapID)
            return false
        }

        self.tapID = createdTapID
        self.aggregateDeviceID = createdAggID
        self.tapIOProcID = procID
        self.outputUnit = outUnit
        self.sourceDeviceID = sourceDeviceID
        self.targetDeviceID = targetDeviceID
        self.isRunning = true

        installVolumeListeners(on: sourceDeviceID)
        return true
    }

    public func stop() {
        isRunning = false
        removeVolumeListeners()

        if aggregateDeviceID != kAudioObjectUnknown, let procID = tapIOProcID {
            AudioDeviceStop(aggregateDeviceID, procID)
            AudioDeviceDestroyIOProcID(aggregateDeviceID, procID)
            tapIOProcID = nil
        }
        if let outUnit = outputUnit {
            AudioOutputUnitStop(outUnit)
            disposeUnit(outUnit)
            outputUnit = nil
        }
        if aggregateDeviceID != kAudioObjectUnknown {
            AudioHardwareDestroyAggregateDevice(aggregateDeviceID)
            aggregateDeviceID = kAudioObjectUnknown
        }
        if tapID != kAudioObjectUnknown {
            AudioHardwareDestroyProcessTap(tapID)
            tapID = kAudioObjectUnknown
        }
        sourceDeviceID = nil
        targetDeviceID = nil
    }

    fileprivate func refreshGain(from deviceID: AudioDeviceID) {
        let scalar = AudioDeviceManager.readVolumeScalar(deviceID: deviceID)
        let muted = AudioDeviceManager.readMute(deviceID: deviceID)
        let gain = AudioDeviceManager.gainFromScalar(scalar, muted: muted)
        os_unfair_lock_lock(&gainLock)
        currentGain = gain
        os_unfair_lock_unlock(&gainLock)
    }

    private func installVolumeListeners(on deviceID: AudioDeviceID) {
        listenerDeviceID = deviceID
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        var volAddr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeScalar,
            mScope: kAudioObjectPropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        var muteAddr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyMute,
            mScope: kAudioObjectPropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectAddPropertyListener(deviceID, &volAddr, volumePropertyListener, selfPtr)
        AudioObjectAddPropertyListener(deviceID, &muteAddr, volumePropertyListener, selfPtr)
    }

    private func removeVolumeListeners() {
        guard listenerDeviceID != kAudioObjectUnknown else { return }
        let selfPtr = Unmanaged.passUnretained(self).toOpaque()
        var volAddr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyVolumeScalar,
            mScope: kAudioObjectPropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        var muteAddr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyMute,
            mScope: kAudioObjectPropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        AudioObjectRemovePropertyListener(listenerDeviceID, &volAddr, volumePropertyListener, selfPtr)
        AudioObjectRemovePropertyListener(listenerDeviceID, &muteAddr, volumePropertyListener, selfPtr)
        listenerDeviceID = kAudioObjectUnknown
    }

    private func createOutputHALUnit(deviceID: AudioDeviceID) -> AudioUnit? {
        var desc = AudioComponentDescription(
            componentType: kAudioUnitType_Output,
            componentSubType: kAudioUnitSubType_HALOutput,
            componentManufacturer: kAudioUnitManufacturer_Apple,
            componentFlags: 0,
            componentFlagsMask: 0
        )
        guard let comp = AudioComponentFindNext(nil, &desc) else { return nil }
        var unit: AudioUnit?
        guard AudioComponentInstanceNew(comp, &unit) == noErr, let au = unit else { return nil }

        var enableIO: UInt32 = 1
        var disableIO: UInt32 = 0
        AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableIO, 4)
        AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disableIO, 4)

        var devID = deviceID
        guard AudioUnitSetProperty(
            au,
            kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global,
            0,
            &devID,
            UInt32(MemoryLayout<AudioDeviceID>.size)
        ) == noErr else {
            AudioComponentInstanceDispose(au)
            return nil
        }

        var asbd = AudioStreamBasicDescription(
            mSampleRate: 48000.0,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked,
            mBytesPerPacket: 8,
            mFramesPerPacket: 1,
            mBytesPerFrame: 8,
            mChannelsPerFrame: 2,
            mBitsPerChannel: 32,
            mReserved: 0
        )
        guard AudioUnitSetProperty(
            au,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input,
            0,
            &asbd,
            UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        ) == noErr else {
            AudioComponentInstanceDispose(au)
            return nil
        }

        return au
    }

    private func setPreferredBufferSize(deviceID: AudioDeviceID, frames: UInt32) {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyBufferFrameSize,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var size = frames
        AudioObjectSetPropertyData(deviceID, &address, 0, nil, UInt32(MemoryLayout<UInt32>.size), &size)
    }

    private func disposeUnit(_ unit: AudioUnit) {
        AudioUnitUninitialize(unit)
        AudioComponentInstanceDispose(unit)
    }

    fileprivate func handleOutput(
        inNumberFrames: UInt32,
        ioData: UnsafeMutablePointer<AudioBufferList>?
    ) -> OSStatus {
        guard let ioData else { return noErr }
        let buffers = UnsafeMutableAudioBufferListPointer(ioData)
        guard let first = buffers.first, let data = first.mData else { return noErr }
        let sampleCount = Int(inNumberFrames) * 2
        let floatPtr = data.assumingMemoryBound(to: Float.self)

        if isRunning {
            ring.read(into: floatPtr, count: sampleCount)
            os_unfair_lock_lock(&gainLock)
            let gain = currentGain
            os_unfair_lock_unlock(&gainLock)
            if gain != 1.0 {
                for i in 0..<sampleCount {
                    floatPtr[i] *= gain
                }
            }
        } else {
            for i in 0..<sampleCount { floatPtr[i] = 0 }
        }
        return noErr
    }
}

private func volumePropertyListener(
    inObjectID: AudioObjectID,
    inNumberAddresses: UInt32,
    inAddresses: UnsafePointer<AudioObjectPropertyAddress>,
    inClientData: UnsafeMutableRawPointer?
) -> OSStatus {
    guard let inClientData else { return noErr }
    let bridge = Unmanaged<AudioBridge>.fromOpaque(inClientData).takeUnretainedValue()
    bridge.refreshGain(from: inObjectID)
    return noErr
}

private func outputRenderCallback(
    inRefCon: UnsafeMutableRawPointer,
    ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
    inTimeStamp: UnsafePointer<AudioTimeStamp>,
    inBusNumber: UInt32,
    inNumberFrames: UInt32,
    ioData: UnsafeMutablePointer<AudioBufferList>?
) -> OSStatus {
    let bridge = Unmanaged<AudioBridge>.fromOpaque(inRefCon).takeUnretainedValue()
    return bridge.handleOutput(inNumberFrames: inNumberFrames, ioData: ioData)
}
