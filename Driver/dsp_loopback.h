#ifndef HDV_DSP_LOOPBACK_H
#define HDV_DSP_LOOPBACK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HDV_RING_CAPACITY_FRAMES 65536u
#define HDV_CHANNELS 2u

typedef struct {
    float samples[HDV_RING_CAPACITY_FRAMES * HDV_CHANNELS];
    _Atomic uint64_t write_frame;
    _Atomic uint64_t read_frame;
} hdv_ring_buffer_t;

void hdv_ring_init(hdv_ring_buffer_t *rb);
void hdv_ring_reset(hdv_ring_buffer_t *rb);

/* Convert macOS scalar [0.0, 1.0] + mute flag to perceptual linear gain. */
float hdv_gain_from_scalar(float scalar, bool muted);

/* Write interleaved Float32 frames into the loopback ring buffer with gain applied. */
void hdv_write_and_scale(
    hdv_ring_buffer_t *rb,
    const float *in_samples,
    uint32_t frame_count,
    uint32_t channels,
    float scalar,
    bool muted
);

/* Read interleaved Float32 frames from the loopback ring buffer; zero-fills underruns. */
void hdv_read_loopback(
    hdv_ring_buffer_t *rb,
    float *out_samples,
    uint32_t frame_count,
    uint32_t channels
);

#ifdef __cplusplus
}
#endif

#endif /* HDV_DSP_LOOPBACK_H */
