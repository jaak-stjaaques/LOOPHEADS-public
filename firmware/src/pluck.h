/* SPDX-License-Identifier: GPL-3.0-only */
#define PLUCK_VOICES 8u
#define PLUCK_RING 512u
#define PLUCK_CHORD_RING 2048u

enum { PLUCK_BRIGHTNESS, PLUCK_POSITION, PLUCK_MACROS };
enum { RESONATOR_STRING, RESONATOR_TUBE, RESONATOR_BLOW };
enum { PING_WHITE, PING_PINK, PING_BROWN, PING_SINE, PING_TRIANGLE,
       PING_SAW, PING_SQUARE, PING_MAJOR, PING_MINOR, PING_SUS2, PING_SUS4, PING_COUNT };
#define PLUCK_ENV_FULL (32767 * 4096)
enum { PLUCK_ENV_ATTACK, PLUCK_ENV_DECAY, PLUCK_ENV_SUSTAIN, PLUCK_ENV_RELEASE };

typedef struct {
    uint16_t attack_ms, decay_ms, release_ms;
    uint8_t sustain;
} pluck_adsr_t;
static const char *const pluck_names[PLUCK_MACROS] = {
    "BRIGHTNESS", "POSITION"
};
static const char *const resonator_names[] = {"STRING", "TUBE", "BLOW"};
static const char *const ping_names[PING_COUNT] = {
    "WHITE NOISE", "PINK NOISE", "BROWN NOISE", "SINE", "TRIANGLE", "SAW", "SQUARE",
    "MAJOR", "MINOR", "SUS2", "SUS4"
};
enum { LFO_PITCH, LFO_VOLUME, LFO_BRIGHTNESS, LFO_POSITION, LFO_TARGETS };
enum { LFO_SINE, LFO_TRIANGLE, LFO_SQUARE, LFO_SAW, LFO_HOLD, LFO_WAVES };
#define LFO_PHASE_ONE (1u << 24)

typedef struct {
    volatile uint32_t increment;
    uint8_t rate_tenths;
    volatile uint8_t target, wave, depth;
} pluck_lfo_t;

typedef struct {
    uint8_t macro[PLUCK_MACROS];
    uint8_t resonator, ping, level;
    volatile int8_t pan;              /* -100 (left) to 100 (right) */
    volatile uint8_t muted;
    volatile uint16_t target_gain;
    pluck_adsr_t adsr;
    pluck_lfo_t lfo;
} pluck_part_t;

#define PLUCK_MAX_LEVEL 127u
#define PLUCK_GAIN_ONE 32768u
static pluck_part_t pluck_parts[TAPE_COUNT];

/* An audio taper with more audible travel between mute and the middle. */
static uint16_t pluck_level_gain(uint8_t level)
{
    static const struct { uint8_t level; uint16_t gain; } points[] = {
        {0, 0}, {1, 130}, {4, 654}, {13, 2603}, {32, 7336},
        {64, 13045}, {96, 20675}, {PLUCK_MAX_LEVEL, PLUCK_GAIN_ONE}
    };
    for (uint32_t i = 1; i < sizeof points / sizeof points[0]; i++) {
        if (level > points[i].level) continue;
        uint32_t span = points[i].level - points[i - 1].level;
        uint32_t step = level - points[i - 1].level;
        uint32_t rise = points[i].gain - points[i - 1].gain;
        return (uint16_t)(points[i - 1].gain + (rise * step + span / 2u) / span);
    }
    return PLUCK_GAIN_ONE;
}

static void pluck_set_level(pluck_part_t *part, uint8_t level)
{
    part->level = level;
    part->target_gain = pluck_level_gain(level);
}

typedef struct {
    int16_t ring[PLUCK_CHORD_RING];
    uint32_t age, phase;
    uint32_t decay_frames, release_frames, env_remaining;
    int32_t envelope, env_step, env_target, sustain;
    int32_t dark_first, dark_second;
    uint32_t breath_noise;
    uint16_t breath_level;
    uint8_t env_stage;
    uint16_t length, cursor, fraction, damping, gain;
    int16_t previous, sample, feedback;
    uint8_t rate_shift, owner, note, part;
    volatile uint8_t active, released;
} pluck_voice_t;

static pluck_voice_t pluck_voices[PLUCK_VOICES];
static uint32_t voice_age, noise = 0x7a817fedu;
static volatile uint32_t tuning_q16 = 65536u;

static void pluck_set_tuning(int32_t cents)
{
    /* Second-order approximation to 2^(cents/1200), accurate over ±50 cents. */
    tuning_q16 = (uint32_t)(65536 + cents * 1893 / 50 + cents * cents * 27 / 2500);
}
#if BOARD_SMK37
static volatile uint32_t bend_q16 = 65536u;
#endif

/* The delay lengths for C9–B9 at 44.1 kHz, in 1/256 sample units. */
static const uint16_t period_c9[] = {
    1348, 1273, 1201, 1134, 1070, 1010, 954, 900, 849, 802, 757, 714
};

static int32_t ping_noise(void)
{
    noise ^= noise << 13;
    noise ^= noise >> 17;
    noise ^= noise << 5;
    return (int32_t)(noise >> 16) - 32768;
}

static int32_t ping_wave(uint8_t ping, uint32_t phase)
{
    int32_t x = (int32_t)phase - 32768;
    int32_t magnitude = x < 0 ? -x : x;
    switch (ping) {
    case PING_SINE:     return x * (32768 - magnitude) / 8192;
    case PING_TRIANGLE: return 32767 - 2 * magnitude;
    case PING_SAW:      return x;
    default:           return phase < 32768u ? 32767 : -32768;
    }
}

/* Generate four chord wavetable frames from three modes of one delay. The
 * first mode is the played pitch. Tube and Blow use odd modes because they
 * invert each trip. */
static const uint8_t chord_modes[PING_COUNT][2][3] = {
    [PING_MAJOR] = {{ 4,  5,  6}, {37, 47, 55}},
    [PING_MINOR] = {{10, 12, 15}, {41, 49, 61}},
    [PING_SUS2]  = {{ 8,  9, 12}, {49, 55, 73}},
    [PING_SUS4]  = {{ 6,  8,  9}, {41, 55, 61}},
};

static int32_t chord_wave(const uint8_t modes[3], uint32_t phase)
{
    int32_t root = ping_wave(PING_SINE, (phase * modes[0]) >> 16);
    int32_t middle = ping_wave(PING_SINE, (phase * modes[1]) >> 16);
    int32_t fifth = ping_wave(PING_SINE, (phase * modes[2]) >> 16);
    return (2 * root + middle + fifth) / 4;
}

static struct {
    volatile uint32_t phase, random;
} pluck_lfo_state[TAPE_COUNT];

static void pluck_lfo_rate(pluck_lfo_t *lfo, uint8_t rate_tenths)
{
    lfo->rate_tenths = rate_tenths;
    lfo->increment = (uint32_t)rate_tenths * LFO_PHASE_ONE / (FS * 10u);
}

static int32_t pluck_lfo_wave(uint8_t wave, uint32_t phase, int32_t held)
{
    if (wave == LFO_HOLD) return held;
    static const uint8_t ping[] = {PING_SINE, PING_TRIANGLE, PING_SQUARE, PING_SAW};
    int32_t sample = ping_wave(ping[wave], phase);
    return sample < -32768 ? -32768 : sample;
}

static int32_t pluck_lfo_value(uint8_t part, uint8_t wave)
{
    return pluck_lfo_wave(wave, pluck_lfo_state[part].phase >> 8,
                          (int32_t)(pluck_lfo_state[part].random >> 16) - 32768);
}

static void pluck_lfo_advance(uint32_t part, uint32_t frames)
{
    uint32_t phase = pluck_lfo_state[part].phase + pluck_parts[part].lfo.increment * frames;
    if (phase >= LFO_PHASE_ONE)
        pluck_lfo_state[part].random = pluck_lfo_state[part].random * 1664525u + 1013904223u + part;
    pluck_lfo_state[part].phase = phase & (LFO_PHASE_ONE - 1u);
}

static int32_t pluck_ping_at(const int16_t *ping, int32_t index, int32_t size, uint8_t tube)
{
    if (index >= 0) return ping[index];
    /* Tube's delay contains half a cycle, with opposite polarity across its seam. */
    return tube ? -ping[index + size] : ping[index + size];
}

static void pluck_position(int16_t *ring, const int16_t *ping, int32_t size,
                           uint8_t tube, uint8_t position, uint8_t root_mode)
{
    /* Move from roughly 8% of the played pitch's cycle to its midpoint. */
    uint32_t offset = (uint32_t)size * (20u * 127u + 108u * position) / 127u;
    if (tube) offset *= 2u;
    offset /= root_mode;
    int32_t whole = (int32_t)(offset >> 8);
    int32_t fraction = (int32_t)(offset & 255u);
    for (int32_t i = 0; i < size; i++) {
        int32_t index = i - whole;
        int32_t delayed = pluck_ping_at(ping, index, size, tube);
        if (fraction) {
            int32_t earlier = pluck_ping_at(ping, index - 1, size, tube);
            delayed += (earlier - delayed) * fraction / 256;
        }
        ring[i] = (int16_t)((ping[i] - delayed) / 2);
    }
}

static void pluck_fill(pluck_voice_t *v, uint8_t velocity, uint8_t tube, uint8_t ping,
                       uint8_t position, const uint8_t *modes)
{
    static int16_t excitation[PLUCK_CHORD_RING];
    uint32_t samples = (uint32_t)v->length + 1u;
    int32_t fast = 0, mid = 0, slow = 0, brown = 0;
    int32_t previous = 0;
    uint32_t phase = 0;
    /* Chord frames use Q32 phase to keep their higher modes aligned at the seam.
     * An inverted repeat supplies Tube and Blow with the other half-cycle. */
    uint32_t phase_step = modes ? (tube ? 0x80000000u / samples : UINT32_MAX / samples + 1u)
                                : (tube ? 32768u : 65536u) / samples;

    for (uint32_t i = 0; i < samples; i++) {
        int32_t signal;
        if (ping >= PING_SINE) {
            signal = modes ? chord_wave(modes, phase) : ping_wave(ping, phase);
            phase += phase_step;
        } else {
            signal = ping_noise();
            if (ping == PING_PINK) {
                /* Blend smoothings at different speeds for an approximate pink spectrum. */
                fast += (signal - fast) / 2;
                mid += (signal - mid) / 8;
                slow += (signal - slow) / 32;
                signal = (signal + 2 * fast + 3 * mid + 5 * slow) / 8;
            } else if (ping == PING_BROWN) {
                brown += (signal - brown) / 16;
                signal = brown * 3;
            }
        }
        if (signal < -32768) signal = -32768;
        if (signal > 32767) signal = 32767;
        int32_t impulse = signal * velocity / 512;
        int32_t sample = (impulse * 65 + previous * 63) / 128;
        excitation[i] = (int16_t)sample;
        previous = impulse;
    }
    pluck_position(v->ring, excitation, (int32_t)samples, tube, position, modes ? modes[0] : 1u);
}

static void pluck_env_ramp(pluck_voice_t *v, uint8_t stage, int32_t target, uint32_t frames)
{
    v->env_stage = stage;
    v->env_remaining = frames;
    v->env_target = target;
    v->env_step = frames ? (target - v->envelope) / (int32_t)frames : 0;
    if (!frames) v->envelope = target;
}

static void pluck_env_decay(pluck_voice_t *v)
{
    uint32_t frames = v->sustain == PLUCK_ENV_FULL ? 0 : v->decay_frames;
    pluck_env_ramp(v, frames ? PLUCK_ENV_DECAY : PLUCK_ENV_SUSTAIN, v->sustain, frames);
}

static inline int pluck_env_tick(pluck_voice_t *v)
{
    if (v->released && v->env_stage != PLUCK_ENV_RELEASE)
        pluck_env_ramp(v, PLUCK_ENV_RELEASE, 0, v->envelope ? v->release_frames : 0);

    if (v->env_stage == PLUCK_ENV_ATTACK && !v->env_remaining) pluck_env_decay(v);
    if (v->env_remaining) {
        v->envelope += v->env_step;
        if (!--v->env_remaining) {
            v->envelope = v->env_target;
            if (v->env_stage == PLUCK_ENV_DECAY) v->env_stage = PLUCK_ENV_SUSTAIN;
        }
    }
    return v->env_stage != PLUCK_ENV_RELEASE || v->env_remaining;
}

static uint16_t pluck_brightness_damping(uint8_t brightness)
{
    /* Open the full filter range while keeping the usual starting tone at 80. */
    if (brightness <= 80u) return (uint16_t)(128u - (93u * brightness + 40u) / 80u);
    return (uint16_t)((35u * (127u - brightness) + 23u) / 47u);
}

static void pluck_start(uint8_t owner, uint8_t part, uint8_t note, uint8_t velocity,
                        uint16_t gain)
{
    const pluck_part_t *sound = &pluck_parts[part];
    pluck_voice_t *v = &pluck_voices[0];
    for (uint32_t i = 0; i < PLUCK_VOICES; i++) {
        pluck_voice_t *candidate = &pluck_voices[i];
        if (candidate->active && candidate->owner == owner && candidate->note == note) {
            v = candidate;
            break;
        }
        if (!candidate->active || (v->active && candidate->age < v->age))
            v = candidate;
    }

    /* Mute this voice while the main loop fills its delay with a burst of noise. */
    fm1_irq_off();
    v->active = 0;
    fm1_irq_on();

    uint32_t period = (uint32_t)period_c9[note % 12u] << (10u - note / 12u);
    uint8_t tube = sound->resonator != RESONATOR_STRING;
    const uint8_t *modes = 0;
    uint32_t ring_size = PLUCK_RING;
    if (sound->ping >= PING_MAJOR) {
        modes = chord_modes[sound->ping][tube];
        period *= modes[0];
        ring_size = PLUCK_CHORD_RING;
    }
    /* String repeats after one trip. Tube repeats after two trips with opposite signs. */
    if (tube) period >>= 1;
    uint8_t rate_shift = 0;
    /* Very low notes update their delay less often, keeping the same memory cost. */
    while (period > (ring_size - 1u) * 256u) {
        rate_shift++;
        period >>= 1;
    }
    v->damping = pluck_brightness_damping(sound->macro[PLUCK_BRIGHTNESS]);
    period -= v->damping;    /* compensate for the smoothing filter's delay */
    v->length = (uint16_t)(period >> 8);
    v->fraction = (uint16_t)(period & 255u);
    /* Keep the longest resonator decay. ENV shapes the note's amplitude. */
    int32_t feedback = 31000 + 13 * 127;
    /* Tube takes two trips per period, so apply about half the loss per trip. */
    if (tube) feedback = -(feedback + (32768 - feedback) / 2);
    v->feedback = (int16_t)feedback;
    v->breath_level = sound->resonator == RESONATOR_BLOW ? velocity * 32u : 0;
    v->breath_noise = 0x9e3779b9u ^ voice_age;
    int32_t position = sound->macro[PLUCK_POSITION];
    if (sound->lfo.target == LFO_POSITION && sound->lfo.depth)
        position += pluck_lfo_value(part, sound->lfo.wave) * sound->lfo.depth / (100 * 512);
    position = position < 0 ? 0 : position > 127 ? 127 : position;
    pluck_fill(v, velocity, tube, sound->ping, (uint8_t)position, modes);
    v->cursor = 0;
    v->phase = 65536u;
    v->previous = v->sample = 0;
    v->dark_first = v->dark_second = 0;
    v->rate_shift = rate_shift;
    v->owner = owner;
    v->part = part;
    v->age = ++voice_age;
    v->note = note;
    v->decay_frames = FS * sound->adsr.decay_ms / 1000u;
    v->release_frames = FS * sound->adsr.release_ms / 1000u;
    v->sustain = (int32_t)(32767u * sound->adsr.sustain / 100u) * 4096;
    if (sound->adsr.attack_ms) {
        v->envelope = 0;
        pluck_env_ramp(v, PLUCK_ENV_ATTACK, PLUCK_ENV_FULL, FS * sound->adsr.attack_ms / 1000u);
    } else {
        v->envelope = PLUCK_ENV_FULL;
        pluck_env_decay(v);
    }
    v->gain = gain;
    v->released = 0;

    fm1_irq_off();
    v->active = 1;
    fm1_irq_on();
}

static void pluck_stop(uint8_t owner, uint8_t note)
{
    for (uint32_t i = 0; i < PLUCK_VOICES; i++)
        if (pluck_voices[i].owner == owner && pluck_voices[i].note == note)
            pluck_voices[i].released = 1;
}

static inline void pluck_resonate(pluck_voice_t *v, uint16_t damping)
{
    uint32_t next = v->cursor == v->length ? 0u : v->cursor + 1u;
    int32_t delayed = (v->ring[next] * (256 - v->fraction) +
                       v->ring[v->cursor] * v->fraction) >> 8;
    int32_t filtered = (delayed * (256 - damping) +
                        v->previous * damping) >> 8;
    v->previous = (int16_t)delayed;
    int32_t next_sample = filtered * v->feedback / 32768;
    if (v->breath_level) {
        /* A reed adds energy at low amplitude and reduces its drive as it opens. */
        int32_t magnitude = filtered < 0 ? -filtered : filtered;
        int32_t drive = filtered * ((int32_t)v->breath_level - magnitude) / 16384;
        v->breath_noise = v->breath_noise * 1664525u + 1013904223u;
        int32_t breath = ((int32_t)(v->breath_noise >> 24) - 128) * v->breath_level / 16384;
        next_sample = -filtered - drive + breath;
    }
    v->ring[v->cursor] = (int16_t)next_sample;
    v->cursor = (uint16_t)next;
    v->sample = (int16_t)filtered;
}

static void pluck_mix_voice(pluck_voice_t *v, int32_t *mix, uint32_t frames,
                            uint32_t rate, int32_t pitch, int32_t brightness)
{
    uint32_t step = rate >> v->rate_shift;
    step = step * (uint32_t)(32768 + pitch) / 32768u;
    int32_t damping = v->damping - brightness;
    damping = damping < 0 ? 0 : damping > 128 ? 128 : damping;
    /* Add two low-pass stages at the dark end without changing the delay length. */
    int32_t dark_coefficient = damping > 88 ? 256 - (damping - 88) * 232 / 40 : 256;
    for (uint32_t frame = 0; frame < frames; frame++) {
        while (v->phase >= 65536u) {
            pluck_resonate(v, (uint16_t)damping);
            v->phase -= 65536u;
        }
        v->phase += step;
        if (!pluck_env_tick(v)) {
            v->active = 0;
            break;
        }
        int32_t tone = v->sample;
        if (dark_coefficient < 256) {
            v->dark_first += (tone * 256 - v->dark_first) * dark_coefficient / 256;
            v->dark_second += (v->dark_first - v->dark_second) * dark_coefficient / 256;
            tone = v->dark_second / 256;
        }
        int32_t sample = tone * (v->envelope >> 12) / 65536;
        /* Match the sustained reed's output to the plucked resonators near note-on. */
        if (v->breath_level) sample /= 4;
        if (v->gain != PLUCK_GAIN_ONE) sample = sample * v->gain / (int32_t)PLUCK_GAIN_ONE;
        mix[frame] += sample;
    }
    if (dark_coefficient == 256) v->dark_first = v->dark_second = v->sample * 256;
}

#define PLUCK_PAN_MAX 100
#define PLUCK_PAN_SCALE 256

static void pluck_mix(int32_t *out, uint32_t frames)
{
    static int32_t mix[HALF_FRAMES];
    static int32_t current_gain[TAPE_COUNT], current_pan[TAPE_COUNT];
    uint32_t rate = tuning_q16;
#if BOARD_SMK37
    rate = (uint32_t)((uint64_t)rate * bend_q16 >> 16);
#endif
    for (uint32_t part = 0; part < TAPE_COUNT; part++) {
        const pluck_lfo_t *lfo = &pluck_parts[part].lfo;
        int32_t target = pluck_parts[part].muted ? 0 : pluck_parts[part].target_gain;
        int32_t pan_target = pluck_parts[part].pan * PLUCK_PAN_SCALE;
        uint8_t depth = lfo->depth, destination = lfo->target;
        int32_t wave = depth ? pluck_lfo_value((uint8_t)part, lfo->wave) : 0;
        int32_t modulation = wave * depth / 100;
        int32_t pitch = destination == LFO_PITCH ? modulation / 8 : 0;
        int32_t brightness = destination == LFO_BRIGHTNESS ? modulation / 1024 : 0;
        if (destination == LFO_VOLUME) {
            int32_t attenuation = (32767 - wave) * depth / 200;
            target -= target * attenuation / 32767;
        }
        pluck_lfo_advance(part, frames);
        uint8_t active = 0;
        for (uint32_t i = 0; i < PLUCK_VOICES; i++) {
            pluck_voice_t *voice = &pluck_voices[i];
            if (!voice->active || voice->part != part) continue;
            if (!active) {
                for (uint32_t frame = 0; frame < frames; frame++) mix[frame] = 0;
                active = 1;
            }
            pluck_mix_voice(voice, mix, frames, rate, pitch, brightness);
        }
        if (!active) {
            current_gain[part] = target;
            current_pan[part] = pan_target;
            continue;
        }
        int32_t gain = current_gain[part], pan = current_pan[part];
        for (uint32_t frame = 0; frame < frames; frame++) {
            /* Apply gain after mixing this tape's voices. Smooth a full sweep over 256 samples. */
            int32_t delta = target - gain;
            gain += delta < -128 ? -128 : delta > 128 ? 128 : delta;
            int32_t sample = mix[frame] * gain / (int32_t)PLUCK_GAIN_ONE;
            int32_t pan_delta = pan_target - pan;
            pan += pan_delta < -128 ? -128 : pan_delta > 128 ? 128 : pan_delta;
            /* Balance preserves the existing centered level. Only the far side is attenuated. */
            int32_t left = sample, right = sample;
            if (pan > 0) left = sample * (PLUCK_PAN_MAX * PLUCK_PAN_SCALE - pan) /
                               (PLUCK_PAN_MAX * PLUCK_PAN_SCALE);
            if (pan < 0) right = sample * (PLUCK_PAN_MAX * PLUCK_PAN_SCALE + pan) /
                                (PLUCK_PAN_MAX * PLUCK_PAN_SCALE);
            out[2u * frame] += left;
            out[2u * frame + 1u] += right;
        }
        current_gain[part] = gain;
        current_pan[part] = pan;
    }
}
