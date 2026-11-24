import AudioToolbox
import CoreAudio
import Foundation

/// Bridges audio from the virtual HDMI Volume loopback input to a physical output device.
public final class AudioBridge: @unchecked Sendable {
    private var inputUnit: AudioUnit?
    private var outputUnit: AudioUnit?
    private let ring = FloatRingBuffer(capacityFrames: 16384, channels: 2)
    private var inputScratch = [Float](repeating: 0, count: 4096 * 2)
    private var isRunning = false

    public private(set) var sourceDeviceID: AudioDeviceID?
    public private(set) var targetDeviceID: AudioDeviceID?

    public init() {}

    deinit {
        stop()
    }

    public func start(sourceDeviceID: AudioDeviceID, targetDeviceID: AudioDeviceID) -> Bool {
        stop()
        ring.reset()

        guard let inUnit = createHALUnit(deviceID: sourceDeviceID, isInput: true),
              let outUnit = createHALUnit(deviceID: targetDeviceID, isInput: false) else {
            return false
        }

        let selfPtr = Unmanaged.passUnretained(self).toOpaque()

        var inCallback = AURenderCallbackStruct(
            inputProc: inputRenderCallback,
            inputProcRefCon: selfPtr
        )
        guard AudioUnitSetProperty(
            inUnit,
            kAudioOutputUnitProperty_SetInputCallback,
            kAudioUnitScope_Global,
            0,
            &inCallback,
            UInt32(MemoryLayout<AURenderCallbackStruct>.size)
        ) == noErr else {
            disposeUnit(inUnit)
            disposeUnit(outUnit)
            return false
        }

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
            disposeUnit(inUnit)
            disposeUnit(outUnit)
            return false
        }

        guard AudioUnitInitialize(inUnit) == noErr,
              AudioUnitInitialize(outUnit) == noErr,
              AudioOutputUnitStart(inUnit) == noErr,
              AudioOutputUnitStart(outUnit) == noErr else {
            disposeUnit(inUnit)
            disposeUnit(outUnit)
            return false
        }

        self.inputUnit = inUnit
        self.outputUnit = outUnit
        self.sourceDeviceID = sourceDeviceID
        self.targetDeviceID = targetDeviceID
        self.isRunning = true
        return true
    }

    public func stop() {
        isRunning = false
        if let inUnit = inputUnit {
            AudioOutputUnitStop(inUnit)
            disposeUnit(inUnit)
            inputUnit = nil
        }
        if let outUnit = outputUnit {
            AudioOutputUnitStop(outUnit)
            disposeUnit(outUnit)
            outputUnit = nil
        }
        sourceDeviceID = nil
        targetDeviceID = nil
    }

    private func createHALUnit(deviceID: AudioDeviceID, isInput: Bool) -> AudioUnit? {
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
        if isInput {
            AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enableIO, 4)
            AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disableIO, 4)
        } else {
            AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableIO, 4)
            AudioUnitSetProperty(au, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disableIO, 4)
        }

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
        let scope: AudioUnitScope = isInput ? kAudioUnitScope_Output : kAudioUnitScope_Input
        let bus: AudioUnitElement = isInput ? 1 : 0
        guard AudioUnitSetProperty(
            au,
            kAudioUnitProperty_StreamFormat,
            scope,
            bus,
            &asbd,
            UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        ) == noErr else {
            AudioComponentInstanceDispose(au)
            return nil
        }

        return au
    }

    private func disposeUnit(_ unit: AudioUnit) {
        AudioUnitUninitialize(unit)
        AudioComponentInstanceDispose(unit)
    }

    fileprivate func handleInput(
        ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
        inTimeStamp: UnsafePointer<AudioTimeStamp>,
        inBusNumber: UInt32,
        inNumberFrames: UInt32
    ) -> OSStatus {
        guard isRunning, let inUnit = inputUnit else { return noErr }
        let sampleCount = Int(inNumberFrames) * 2
        if inputScratch.count < sampleCount {
            inputScratch = [Float](repeating: 0, count: sampleCount)
        }

        return inputScratch.withUnsafeMutableBufferPointer { buf in
            var abl = AudioBufferList(
                mNumberBuffers: 1,
                mBuffers: AudioBuffer(
                    mNumberChannels: 2,
                    mDataByteSize: UInt32(sampleCount * MemoryLayout<Float>.size),
                    mData: buf.baseAddress
                )
            )
            let status = AudioUnitRender(inUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, &abl)
            if status == noErr, let base = buf.baseAddress {
                ring.write(base, count: sampleCount)
            }
            return status
        }
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
        } else {
            for i in 0..<sampleCount { floatPtr[i] = 0 }
        }
        return noErr
    }
}

private func inputRenderCallback(
    inRefCon: UnsafeMutableRawPointer,
    ioActionFlags: UnsafeMutablePointer<AudioUnitRenderActionFlags>,
    inTimeStamp: UnsafePointer<AudioTimeStamp>,
    inBusNumber: UInt32,
    inNumberFrames: UInt32,
    ioData: UnsafeMutablePointer<AudioBufferList>?
) -> OSStatus {
    let bridge = Unmanaged<AudioBridge>.fromOpaque(inRefCon).takeUnretainedValue()
    return bridge.handleInput(
        ioActionFlags: ioActionFlags,
        inTimeStamp: inTimeStamp,
        inBusNumber: inBusNumber,
        inNumberFrames: inNumberFrames
    )
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
