/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#define DINGHY_HOST 1
#define FS 44100u
#include "effects.h"

static int32_t echo_peak(const int32_t *buffer, uint32_t from, uint32_t to)
{
    int32_t peak = 0;
    for (uint32_t frame = from; frame < to; frame++)
        if (buffer[2 * frame] > peak) peak = buffer[2 * frame];
    return peak;
}

static void check_feedback(void)
{
    int32_t buffer[2 * 1000] = {0};
    delay_configure(1, 100, 50, 100);
    effects_process(buffer, 256); /* allow the mix fade-in to settle */
    for (uint32_t i = 0; i < 64; i++) buffer[i] = 12000;
    effects_process(buffer, 1000);
    for (uint32_t i = 0; i < 2 * 220; i++) assert(!buffer[i]);
    int32_t first = echo_peak(buffer, 220, 256);
    assert(first > 11900 && first <= 12000);
    assert(echo_peak(buffer, 440, 480) == 16 * (first / 32));
    assert(echo_peak(buffer, 661, 700) == 16 * (first / 64));
}

static void check_bypass(void)
{
    int32_t buffer[2 * 512] = {0};
    delay_configure(0, 100, 50, 100);
    effects_process(buffer, 512);
    for (uint32_t i = 0; i < 2 * 512; i++) buffer[i] = 0;
    buffer[0] = 5000;
    buffer[1] = -7000;
    effects_process(buffer, 512);
    assert(buffer[0] == 5000 && buffer[1] == -7000);
    delay_configure(1, 100, 50, 0);
    effects_process(buffer, 512);
    assert(buffer[0] == 5000 && buffer[1] == -7000);
    for (uint32_t i = 2; i < 2 * 512; i++) assert(!buffer[i]);
}

static void check_sample_format(void)
{
    int32_t loud[2 * 100] = {0};
    delay_configure(0, 100, 0, 0);
    effects_process(loud, 100);
    delay_configure(1, 100, 85, 0);
    for (uint32_t at = 0; at < FS; at += 100) {
        for (uint32_t i = 0; i < 200; i++) loud[i] = 32767;
        effects_process(loud, 100);
    }
    assert(delay_state.clock.phase == 0 && delay_state.valid == 20000);
    assert(delay_ring[(delay_state.head + DELAY_CAPACITY - 1) % DELAY_CAPACITY] == 2047);
}

int main(void)
{
    assert(delay_tempo_samples(30, 36) == DELAY_CAPACITY);
    assert(delay_tempo_samples(120, 24) == 10000);
    assert(delay_tempo_samples(100, 24) == 12000);
    check_feedback();
    check_bypass();
    check_sample_format();
    return 0;
}
