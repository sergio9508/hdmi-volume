import Foundation

/// Lock-protected stereo Float32 ring buffer with bounded max latency to prevent standing audio delay.
public final class FloatRingBuffer: @unchecked Sendable {
    private var buffer: [Float]
    private let capacitySamples: Int
    private let maxLatencySamples: Int
    private var writeIndex: Int = 0
    private var readIndex: Int = 0
    private var availableSamples: Int = 0
    private var lock = os_unfair_lock_s()

    public init(capacityFrames: Int = 4096, maxLatencyFrames: Int = 768, channels: Int = 2) {
        let cap = max(1, capacityFrames * channels)
        self.capacitySamples = cap
        self.maxLatencySamples = min(cap, max(channels, maxLatencyFrames * channels))
        self.buffer = [Float](repeating: 0, count: cap)
    }

    public func reset() {
        os_unfair_lock_lock(&lock)
        writeIndex = 0
        readIndex = 0
        availableSamples = 0
        os_unfair_lock_unlock(&lock)
    }

    public func write(_ source: UnsafePointer<Float>, count: Int) {
        guard count > 0 else { return }
        os_unfair_lock_lock(&lock)
        if count >= maxLatencySamples {
            let start = count - maxLatencySamples
            for i in 0..<maxLatencySamples {
                buffer[i] = source[start + i]
            }
            readIndex = 0
            writeIndex = maxLatencySamples % capacitySamples
            availableSamples = maxLatencySamples
        } else {
            let overflow = (availableSamples + count) - maxLatencySamples
            if overflow > 0 {
                readIndex = (readIndex + overflow) % capacitySamples
                availableSamples -= overflow
            }
            for i in 0..<count {
                buffer[(writeIndex + i) % capacitySamples] = source[i]
            }
            writeIndex = (writeIndex + count) % capacitySamples
            availableSamples += count
        }
        os_unfair_lock_unlock(&lock)
    }

    public func read(into destination: UnsafeMutablePointer<Float>, count: Int) {
        guard count > 0 else { return }
        os_unfair_lock_lock(&lock)
        let toRead = min(count, availableSamples)
        for i in 0..<toRead {
            destination[i] = buffer[(readIndex + i) % capacitySamples]
        }
        if toRead < count {
            for i in toRead..<count {
                destination[i] = 0
            }
        }
        readIndex = (readIndex + toRead) % capacitySamples
        availableSamples -= toRead
        os_unfair_lock_unlock(&lock)
    }
}
