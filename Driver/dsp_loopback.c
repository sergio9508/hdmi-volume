#include "dsp_loopback.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

void hdv_ring_init(hdv_ring_buffer_t *rb) {
    if (!rb) return;
    memset(rb->samples, 0, sizeof(rb->samples));
    atomic_store_explicit(&rb->write_frame, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->read_frame, 0, memory_order_relaxed);
}

void hdv_ring_reset(hdv_ring_buffer_t *rb) {
    hdv_ring_init(rb);
}

float hdv_gain_from_scalar(float scalar, bool muted) {
    if (muted || scalar <= 0.0f) {
        return 0.0f;
    }
    if (scalar >= 1.0f) {
        return 1.0f;
    }
    /* Perceptual -60 dB to 0 dB curve matching macOS volume feel. */
    float db = (scalar - 1.0f) * 60.0f;
    return powf(10.0f, db / 20.0f);
}

void hdv_write_and_scale(
    hdv_ring_buffer_t *rb,
    const float *in_samples,
    uint32_t frame_count,
    uint32_t channels,
    float scalar,
    bool muted
) {
    if (!rb || !in_samples || frame_count == 0 || channels == 0) return;

    float gain = hdv_gain_from_scalar(scalar, muted);
    uint64_t w = atomic_load_explicit(&rb->write_frame, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&rb->read_frame, memory_order_acquire);

    /* If writer outpaces reader by more than capacity, advance read_frame. */
    if (w - r + frame_count > HDV_RING_CAPACITY_FRAMES) {
        uint64_t new_r = (w + frame_count) - HDV_RING_CAPACITY_FRAMES;
        atomic_store_explicit(&rb->read_frame, new_r, memory_order_release);
    }

    uint32_t copy_ch = channels < HDV_CHANNELS ? channels : HDV_CHANNELS;
    for (uint32_t f = 0; f < frame_count; ++f) {
        uint32_t idx = (uint32_t)((w + f) & (HDV_RING_CAPACITY_FRAMES - 1u));
        float *dst = &rb->samples[idx * HDV_CHANNELS];
        const float *src = &in_samples[f * channels];
        for (uint32_t c = 0; c < copy_ch; ++c) {
            dst[c] = src[c] * gain;
        }
        for (uint32_t c = copy_ch; c < HDV_CHANNELS; ++c) {
            dst[c] = 0.0f;
        }
    }

    atomic_store_explicit(&rb->write_frame, w + frame_count, memory_order_release);
}

void hdv_read_loopback(
    hdv_ring_buffer_t *rb,
    float *out_samples,
    uint32_t frame_count,
    uint32_t channels
) {
    if (!out_samples || frame_count == 0 || channels == 0) return;
    if (!rb) {
        memset(out_samples, 0, (size_t)frame_count * channels * sizeof(float));
        return;
    }

    uint64_t w = atomic_load_explicit(&rb->write_frame, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&rb->read_frame, memory_order_relaxed);
    uint64_t available = (w >= r) ? (w - r) : 0;

    uint32_t to_read = (available >= frame_count) ? frame_count : (uint32_t)available;
    uint32_t copy_ch = channels < HDV_CHANNELS ? channels : HDV_CHANNELS;

    for (uint32_t f = 0; f < to_read; ++f) {
        uint32_t idx = (uint32_t)((r + f) & (HDV_RING_CAPACITY_FRAMES - 1u));
        const float *src = &rb->samples[idx * HDV_CHANNELS];
        float *dst = &out_samples[f * channels];
        for (uint32_t c = 0; c < copy_ch; ++c) {
            dst[c] = src[c];
        }
        for (uint32_t c = copy_ch; c < channels; ++c) {
            dst[c] = 0.0f;
        }
    }

    if (to_read < frame_count) {
        size_t rem = (size_t)(frame_count - to_read) * channels * sizeof(float);
        memset(&out_samples[(size_t)to_read * channels], 0, rem);
    }

    atomic_store_explicit(&rb->read_frame, r + to_read, memory_order_release);
}
