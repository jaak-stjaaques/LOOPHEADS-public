/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define BOARD_SMK37 0
#define TAPE_COUNT 4u
#define FS 44100u
#define HALF_FRAMES 256u
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
#include "pluck.h"

static void render(uint8_t level, int32_t out[2 * HALF_FRAMES], int16_t *probe)
{
    memset(pluck_voices, 0, sizeof pluck_voices);
    memset(out, 0, sizeof(int32_t) * 2 * HALF_FRAMES);
    pluck_parts[0].adsr.sustain = pluck_parts[1].adsr.sustain = 100;
    pluck_set_level(&pluck_parts[0], level);
    pluck_set_level(&pluck_parts[1], PLUCK_MAX_LEVEL);
    pluck_mix(out, HALF_FRAMES);

    /* Deterministic pings let us compare identical notes before and after a level change. */
    pluck_parts[0].ping = PING_SINE;
    pluck_parts[0].resonator = RESONATOR_STRING;
    pluck_parts[1].ping = PING_SINE;
    pluck_parts[1].resonator = RESONATOR_TUBE;
    pluck_start(1, 0, 57, 100, PLUCK_GAIN_ONE);
    pluck_start(2, 1, 57, 100, PLUCK_GAIN_ONE);
    const pluck_voice_t *string = 0, *tube = 0;
    for (uint32_t i = 0; i < PLUCK_VOICES; i++) {
        const pluck_voice_t *voice = &pluck_voices[i];
        if (!voice->active) continue;
        if (voice->part == 0) string = voice;
        if (voice->part == 1) tube = voice;
    }
    assert(string && string->feedback > 0);
    assert(tube && tube->feedback < 0);

    pluck_mix(out, HALF_FRAMES);
    *probe = string->sample;
}

static void check_adsr(void)
{
    memset(pluck_voices, 0, sizeof pluck_voices);
    pluck_parts[0].adsr = (pluck_adsr_t){.attack_ms = 10, .decay_ms = 10,
                                       .release_ms = 10, .sustain = 50};
    pluck_parts[1].adsr = (pluck_adsr_t){.sustain = 100};
    pluck_start(1, 0, 57, 100, PLUCK_GAIN_ONE);
    pluck_start(2, 1, 57, 100, PLUCK_GAIN_ONE);
    pluck_voice_t *voice = 0, *other = 0;
    for (uint32_t i = 0; i < PLUCK_VOICES; i++) {
        pluck_voice_t *candidate = &pluck_voices[i];
        if (!candidate->active) continue;
        if (candidate->part == 0) voice = candidate;
        if (candidate->part == 1) other = candidate;
    }
    assert(voice && other && voice->envelope == 0 && other->envelope == PLUCK_ENV_FULL);
    for (uint32_t i = 0; i < 441; i++) assert(pluck_env_tick(voice));
    assert(voice->envelope == PLUCK_ENV_FULL);
    for (uint32_t i = 0; i < 441; i++) assert(pluck_env_tick(voice));
    assert(voice->env_stage == PLUCK_ENV_SUSTAIN && voice->envelope == voice->sustain);
    pluck_stop(1, 57);
    for (uint32_t i = 0; i < 440; i++) assert(pluck_env_tick(voice));
    assert(!pluck_env_tick(voice) && voice->envelope == 0 && !other->released);
}

int main(void)
{
    assert(pluck_level_gain(0) == 0);
    assert(pluck_level_gain(1) == 130);
    assert(pluck_level_gain(13) == 2603); /* displayed 10 is about -22 dB */
    assert(pluck_level_gain(127) == 32768);
    assert(pluck_level_gain(64) == 13045); /* halfway is about -8 dB */
    for (uint8_t level = 1; level <= 127; level++)
        assert(pluck_level_gain(level) > pluck_level_gain(level - 1));

    int32_t full[2 * HALF_FRAMES], quiet[2 * HALF_FRAMES], muted[2 * HALF_FRAMES];
    int16_t full_probe, quiet_probe, muted_probe;
    render(127, full, &full_probe);
    render(13, quiet, &quiet_probe);
    render(0, muted, &muted_probe);
    assert(full_probe == quiet_probe && quiet_probe == muted_probe);
    uint32_t tape_one_audible = 0, tape_two_audible = 0;
    for (uint32_t sample = 0; sample < 2 * HALF_FRAMES; sample++) {
        int32_t tape_one = full[sample] - muted[sample];
        int32_t expected = tape_one * (int32_t)pluck_level_gain(13) / 32768;
        assert(quiet[sample] - muted[sample] == expected);
        tape_one_audible += expected != 0;
        tape_two_audible += muted[sample] != 0;
    }
    assert(tape_one_audible && tape_two_audible);
    check_adsr();
    return 0;
}
