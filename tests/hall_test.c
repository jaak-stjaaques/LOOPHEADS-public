/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define DINGHY_HOST 1
#define FS 44100u
#include "effects.h"

static uint64_t measure_tail(uint8_t decay)
{
    int32_t signal[2 * 256] = {0};
    uint64_t tail = 0;
    uint8_t stereo = 0;
    hall_configure(0, decay, 35, 100);
    effects_process(signal, 256);
    memset(signal, 0, sizeof signal);
    effects_process(signal, 256);
    hall_configure(1, decay, 35, 100);
    effects_process(signal, 256);

    for (uint32_t block = 0; block < 180; block++) {
        memset(signal, 0, sizeof signal);
        if (!block) {
            for (uint32_t frame = 0; frame < 64; frame++)
                signal[2 * frame] = signal[2 * frame + 1] = frame < 32 ? 24000 : -24000;
        }
        effects_process(signal, 256);
        for (uint32_t frame = 0; frame < 256; frame++) {
            int32_t left = signal[2 * frame], right = signal[2 * frame + 1];
            if (block < 8) assert(!left && !right); /* first return is about 53 ms */
            assert(left >= -32768 && left < 32768);
            assert(right >= -32768 && right < 32768);
            stereo |= left != right;
            if (block > 100) tail += (uint32_t)(left * left) + (uint32_t)(right * right);
        }
    }
    assert(stereo);
    return tail;
}

#define IMPULSE_FRAMES 4096u

static void render_impulse(int32_t signal[2 * IMPULSE_FRAMES], uint8_t echo, uint8_t reverb)
{
    memset(&delay_state, 0, sizeof delay_state);
    memset(&hall, 0, sizeof hall);
    delay_configure(echo, 200, 50, 100);
    hall_configure(reverb, 65, 35, 100);
    int32_t settle[2 * 256] = {0};
    effects_process(settle, 256);

    memset(signal, 0, 2 * IMPULSE_FRAMES * sizeof *signal);
    for (uint32_t i = 0; i < 32; i++) {
        signal[2 * i] = 24000;
        signal[2 * i + 1] = 12000;
    }
    for (uint32_t i = 0; i < IMPULSE_FRAMES; i += 256)
        effects_process(&signal[2 * i], 256);
}

static void check_parallel_returns(void)
{
    int32_t echo[2 * IMPULSE_FRAMES], reverb[2 * IMPULSE_FRAMES], both[2 * IMPULSE_FRAMES];
    render_impulse(echo, 1, 0);
    render_impulse(reverb, 0, 1);
    render_impulse(both, 1, 1);

    uint8_t heard_echo = 0, heard_reverb = 0;
    for (uint32_t i = 0; i < 2 * IMPULSE_FRAMES; i++) {
        assert(both[i] == (echo[i] + reverb[i]) / 2);
        heard_echo |= echo[i] != 0;
        heard_reverb |= reverb[i] != 0;
    }
    assert(heard_echo && heard_reverb);
}

int main(void)
{
    int32_t signal[2 * 256] = {0};
    hall_configure(1, 50, 30, 0);
    for (uint32_t at = 0; at < FS;) {
        uint32_t frames = FS - at < 256 ? FS - at : 256;
        effects_process(signal, frames);
        at += frames;
    }
    assert(hall.clock.phase == 0 && hall.tank_head[0] == 20000 % HALL_D1);
    assert(fx12_quantize(-9999) == -2048 && fx12_quantize(9999) == 2047);

    signal[0] = 4321;
    signal[1] = -7654;
    effects_process(signal, 256);
    assert(signal[0] == 4321 && signal[1] == -7654);

    uint64_t short_tail = measure_tail(0);
    uint64_t long_tail = measure_tail(100);
    assert(long_tail > short_tail && long_tail > 0);
    for (uint32_t i = 0; i < sizeof hall_tank / sizeof hall_tank[0]; i++)
        assert(hall_tank[i] >= -2048 && hall_tank[i] <= 2047);
    check_parallel_returns();
    return 0;
}
