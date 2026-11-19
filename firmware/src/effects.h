/* SPDX-License-Identifier: GPL-3.0-only */
#include "delay.h"
#include "hall.h"

static void effects_process(int32_t *out, uint32_t frames)
{
    uint8_t delay_active = delay_begin_block();
    uint8_t hall_active = hall_begin_block();
    if (!delay_active && !hall_active) return;

    for (uint32_t frame = 0; frame < frames; frame++) {
        int32_t *pair = &out[2u * frame];
        int32_t dry = (pair[0] + pair[1]) / 2;
        int32_t echo = delay_active ? delay_render(dry) : 0;
        int32_t left = 0, right = 0;
        if (hall_active) hall_render(dry, &left, &right);

        int32_t delay_mix = delay_state.mix;
        int32_t hall_mix = hall.mix;
        if (!delay_mix && !hall_mix) continue;

        /* Keep one dry path. Divide the wet overlap evenly so the three gains sum to unity. */
        int32_t overlap = delay_mix * hall_mix / FX12_GAIN_ONE;
        int32_t dry_gain = FX12_GAIN_ONE - delay_mix - hall_mix + overlap;
        int32_t echo_return = echo * (delay_mix - overlap / 2);
        int32_t hall_gain = hall_mix - (overlap - overlap / 2);
        pair[0] = (pair[0] * dry_gain + echo_return + left * hall_gain) / FX12_GAIN_ONE;
        pair[1] = (pair[1] * dry_gain + echo_return + right * hall_gain) / FX12_GAIN_ONE;
    }
}
