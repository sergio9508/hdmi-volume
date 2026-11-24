#include "dsp_loopback.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void test_gain_from_scalar(void) {
    assert(hdv_gain_from_scalar(0.0f, false) == 0.0f);
    assert(hdv_gain_from_scalar(0.75f, true) == 0.0f);
    assert(hdv_gain_from_scalar(1.0f, false) == 1.0f);

    float g_half = hdv_gain_from_scalar(0.5f, false);
    /* -30 dB => 10^(-1.5) ~= 0.031622776 */
    assert(fabsf(g_half - 0.031622776f) < 1e-4f);
}

static void test_write_and_read_scaled(void) {
    hdv_ring_buffer_t rb;
    hdv_ring_init(&rb);

    float in_frames[4 * 2] = {
        1.0f, -1.0f,
        0.5f, -0.5f,
        0.25f, -0.25f,
        0.0f, 0.0f
    };
    hdv_write_and_scale(&rb, in_frames, 4, 2, 1.0f, false);

    float out_frames[4 * 2] = {0};
    hdv_read_loopback(&rb, out_frames, 4, 2);
    for (int i = 0; i < 8; ++i) {
        assert(fabsf(out_frames[i] - in_frames[i]) < 1e-6f);
    }
}

static void test_muted_zeroes_output(void) {
    hdv_ring_buffer_t rb;
    hdv_ring_init(&rb);

    float in_frames[2 * 2] = {0.8f, -0.8f, 0.4f, -0.4f};
    hdv_write_and_scale(&rb, in_frames, 2, 2, 1.0f, true);

    float out_frames[2 * 2] = {1.0f, 1.0f, 1.0f, 1.0f};
    hdv_read_loopback(&rb, out_frames, 2, 2);
    for (int i = 0; i < 4; ++i) {
        assert(out_frames[i] == 0.0f);
    }
}

static void test_underrun_zero_fills(void) {
    hdv_ring_buffer_t rb;
    hdv_ring_init(&rb);

    float out_frames[4 * 2];
    for (int i = 0; i < 8; ++i) out_frames[i] = 9.0f;
    hdv_read_loopback(&rb, out_frames, 4, 2);
    for (int i = 0; i < 8; ++i) {
        assert(out_frames[i] == 0.0f);
    }
}

int main(void) {
    test_gain_from_scalar();
    test_write_and_read_scaled();
    test_muted_zeroes_output();
    test_underrun_zero_fills();
    printf("test_dsp_loopback: all 4 tests passed\n");
    return 0;
}
