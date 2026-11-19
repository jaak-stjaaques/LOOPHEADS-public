/* SPDX-License-Identifier: GPL-3.0-only */
/* One user snapshot, written alternately to two sectors so a failed write
 * does not erase the previous snapshot. Runtime state is rebuilt on recall. */
#define SNAPSHOT_MAGIC 0x3153484cu
#define SNAPSHOT_FIRST_BLOB (NBLOB - 2u)

typedef struct {
    uint16_t start, end, reference, leak_loss;
    uint8_t pitch, velocity;
} snapshot_note_t;

typedef struct {
    snapshot_note_t notes[TAPE_MAX_NOTES];
    uint16_t length;
    uint8_t count, mode, leak_passes;
    uint8_t brightness, position, resonator, ping, volume;
    uint16_t attack, decay, release;
    uint8_t sustain, lfo_target, lfo_wave, lfo_rate, lfo_depth;
} snapshot_tape_t;

typedef struct {
    uint32_t magic, generation;
    snapshot_tape_t tape[TAPE_COUNT];
    uint8_t scale, root, armed, octave, channel;
    uint8_t arp_mode, arp_rate, arp_gate, bpm, strum_spread;
    uint8_t delay_on, division, feedback, delay_mix;
    uint8_t hall_on, hall_decay, damping, hall_mix;
    int8_t pan[TAPE_COUNT];
    uint8_t muted[TAPE_COUNT];
} snapshot_t;

/* The original blob ends before the mixer settings, rounded to its struct alignment. */
#define SNAPSHOT_LEGACY_SIZE ((__builtin_offsetof(snapshot_t, pan) + 3u) & ~3u)
_Static_assert(SNAPSHOT_LEGACY_SIZE == 2676, "Original snapshot layout");
_Static_assert(sizeof(snapshot_t) <= SECTOR_MAX, "Snapshot fits in one flash sector");
static snapshot_t snapshot;
static int8_t snapshot_bank = -1;
static uint32_t snapshot_generation;

static int snapshot_valid(void)
{
    if (snapshot.magic != SNAPSHOT_MAGIC ||
        snapshot.scale >= SCALE_COUNT || snapshot.root > 11 || snapshot.armed >= TAPE_COUNT ||
        (int8_t)snapshot.octave < -2 || (int8_t)snapshot.octave > 2 || snapshot.channel > 16 ||
        snapshot.arp_mode >= ARP_MODES || snapshot.arp_rate >= ARP_RATES ||
        snapshot.arp_gate < 10 || snapshot.arp_gate > 100 ||
        snapshot.bpm < 40 || snapshot.bpm > 240 || snapshot.strum_spread < 5 ||
        snapshot.strum_spread > 80 || snapshot.delay_on > 1 ||
        snapshot.division >= sizeof delay_division_clocks || snapshot.feedback > 85 ||
        snapshot.delay_mix > 100 || snapshot.hall_on > 1 || snapshot.hall_decay > 100 ||
        snapshot.damping > 100 || snapshot.hall_mix > 100) return 0;
    for (uint32_t i = 0; i < TAPE_COUNT; i++) {
        const snapshot_tape_t *tape = &snapshot.tape[i];
        if (snapshot.pan[i] < -PLUCK_PAN_MAX || snapshot.pan[i] > PLUCK_PAN_MAX ||
            snapshot.muted[i] > 1 || tape->length < TAPE_MIN_MS || tape->length > TAPE_MAX_MS ||
            tape->count > TAPE_MAX_NOTES || tape->mode > TAPE_STRETCH ||
            tape->leak_passes == 1 || tape->leak_passes > 10 ||
            tape->brightness > 127 || tape->position > 127 ||
            tape->resonator > RESONATOR_BLOW || tape->ping >= PING_COUNT ||
            tape->volume > PLUCK_MAX_LEVEL || tape->attack > 2000 || tape->decay > 2000 ||
            tape->release > 1500 || tape->sustain > 100 || tape->lfo_target >= LFO_TARGETS ||
            tape->lfo_wave >= LFO_WAVES || !tape->lfo_rate || tape->lfo_rate > 120 ||
            tape->lfo_depth > 100) return 0;
        for (uint32_t j = 0; j < tape->count; j++) {
            const snapshot_note_t *note = &tape->notes[j];
            if (note->reference < TAPE_MIN_MS || note->reference > TAPE_MAX_MS ||
                note->start >= note->end || note->end > note->reference ||
                note->pitch > 127 || !note->velocity || note->velocity > 127 ||
                note->leak_loss >= TAPE_LEAK_END) return 0;
        }
    }
    return 1;
}

static int snapshot_load(uint32_t bank)
{
    uint32_t blob = SNAPSHOT_FIRST_BLOB + bank;
    if (blob_load(blob, &snapshot, sizeof snapshot)) {
        if (blob_load(blob, &snapshot, SNAPSHOT_LEGACY_SIZE)) return -1;
        memset(snapshot.pan, 0, sizeof snapshot.pan);
        memset(snapshot.muted, 0, sizeof snapshot.muted);
    }
    return snapshot_valid() ? 0 : -1;
}

static void snapshot_find(void)
{
    snapshot_bank = -1;
    for (uint32_t bank = 0; bank < 2; bank++) {
        if (snapshot_load(bank)) continue;
        if (snapshot_bank < 0 || (int32_t)(snapshot.generation - snapshot_generation) > 0) {
            snapshot_bank = (int8_t)bank;
            snapshot_generation = snapshot.generation;
        }
    }
}

static int snapshot_save(void)
{
    memset(&snapshot, 0, sizeof snapshot);
    snapshot.magic = SNAPSHOT_MAGIC;
    snapshot.generation = snapshot_generation + 1u;
    snapshot.scale = global_scale; snapshot.root = scale_root; snapshot.armed = armed_tape;
    snapshot.octave = set.oct; snapshot.channel = set.ch;
    snapshot.arp_mode = arp.mode; snapshot.arp_rate = arp.rate; snapshot.arp_gate = arp.gate;
    snapshot.bpm = arp.bpm; snapshot.strum_spread = arp.spread_ms;
    snapshot.delay_on = fx.on; snapshot.division = fx.division;
    snapshot.feedback = fx.feedback; snapshot.delay_mix = fx.mix;
    snapshot.hall_on = hall_settings.on; snapshot.hall_decay = hall_settings.decay;
    snapshot.damping = hall_settings.damping; snapshot.hall_mix = hall_settings.mix;
    for (uint32_t i = 0; i < TAPE_COUNT; i++) {
        const tape_t *tape = &tapes[i];
        const pluck_part_t *part = &pluck_parts[i];
        snapshot_tape_t *saved = &snapshot.tape[i];
        saved->length = tape->length_ms; saved->count = tape->count;
        saved->mode = tape->mode; saved->leak_passes = tape->leak_passes;
        saved->brightness = part->macro[PLUCK_BRIGHTNESS]; saved->position = part->macro[PLUCK_POSITION];
        snapshot.pan[i] = part->pan;
        snapshot.muted[i] = part->muted;
        saved->resonator = part->resonator; saved->ping = part->ping; saved->volume = part->level;
        saved->attack = part->adsr.attack_ms; saved->decay = part->adsr.decay_ms;
        saved->release = part->adsr.release_ms; saved->sustain = part->adsr.sustain;
        saved->lfo_target = part->lfo.target; saved->lfo_wave = part->lfo.wave;
        saved->lfo_rate = part->lfo.rate_tenths; saved->lfo_depth = part->lfo.depth;
        for (uint32_t j = 0; j < tape->count; j++) {
            const tape_note_t *note = &tape->notes[j];
            uint16_t end = note->end_ms;
            if (!end) end = tape->position_ms > note->start_ms ? tape->position_ms : note->start_ms + 1u;
            saved->notes[j] = (snapshot_note_t){note->start_ms, end, note->reference_ms,
                                                note->leak_loss, note->pitch, note->velocity};
        }
    }

    uint32_t bank = snapshot_bank == 0 ? 1u : 0u;
    uint32_t generation = snapshot.generation;
    if (!snapshot_valid() || blob_save(SNAPSHOT_FIRST_BLOB + bank, &snapshot, sizeof snapshot) ||
        blob_load(SNAPSHOT_FIRST_BLOB + bank, &snapshot, sizeof snapshot) ||
        !snapshot_valid() || snapshot.generation != generation) return -1;
    snapshot_bank = (int8_t)bank;
    snapshot_generation = generation;
    return 0;
}

static int snapshot_recall(void)
{
    if (snapshot_bank < 0 ||
        snapshot_load((uint32_t)snapshot_bank)) return -1;

    global_scale = snapshot.scale; scale_root = snapshot.root; armed_tape = snapshot.armed;
    transpose_root = NO_TRANSPOSE_ROOT;
    for (uint32_t i = 0; i < TAPE_COUNT; i++) {
        const snapshot_tape_t *saved = &snapshot.tape[i];
        tape_t *tape = &tapes[i];
        pluck_part_t *part = &pluck_parts[i];
        uint8_t was_playing = tape->playing;
        uint16_t position = tape->position_ms;
        for (uint32_t j = 0; j < tape->count; j++) {
            if (tape->notes[j].active) tape_release_note(tape, &tape->notes[j], tape_event);
        }
        memset(tape, 0, sizeof *tape);
        tape->playing = was_playing;
        tape->position_ms = position;
        tape->count = saved->count;
        tape->leak_passes = saved->leak_passes;
        for (uint32_t j = 0; j < tape->count; j++) {
            const snapshot_note_t *note = &saved->notes[j];
            tape->notes[j] = (tape_note_t){.start_ms = note->start, .end_ms = note->end,
                                         .reference_ms = note->reference, .leak_loss = note->leak_loss,
                                         .pitch = note->pitch, .velocity = note->velocity,
                                         .leak_seen_this_pass = 1};
        }
        tape_set_timing(tape, saved->length, saved->mode, tape_event);
        part->macro[PLUCK_BRIGHTNESS] = saved->brightness; part->macro[PLUCK_POSITION] = saved->position;
        part->resonator = saved->resonator; part->ping = saved->ping;
        pluck_set_level(part, saved->volume);
        part->pan = snapshot.pan[i];
        part->muted = snapshot.muted[i];
        part->adsr = (pluck_adsr_t){saved->attack, saved->decay, saved->release, saved->sustain};
        /* Older snapshots allowed longer attack and decay times. */
        if (part->adsr.attack_ms > ENV_MAX_MS) part->adsr.attack_ms = ENV_MAX_MS;
        if (part->adsr.decay_ms > ENV_MAX_MS) part->adsr.decay_ms = ENV_MAX_MS;
        part->lfo.target = saved->lfo_target; part->lfo.wave = saved->lfo_wave;
        pluck_lfo_rate(&part->lfo, saved->lfo_rate);
        part->lfo.depth = saved->lfo_depth;
    }
    /* Bring in the recalled notes that already span each running playhead. */
    for (uint32_t i = 0; i < TAPE_COUNT; i++) {
        tape_t *tape = &tapes[i];
        if (!tape->playing) continue;
        for (uint32_t j = 0; j < tape->count; j++) {
            tape_note_t *note = &tape->notes[j];
            if (note->play_start_ms <= tape->position_ms && tape->position_ms < note->play_end_ms) {
                note->active = 1;
                tape->playing_pitch[note->pitch]++;
            }
        }
    }
    retune_playing_tapes();
    arp.rate = snapshot.arp_rate; arp.gate = snapshot.arp_gate;
    arp.bpm = snapshot.bpm; arp.spread_ms = snapshot.strum_spread;
    if (arp.mode == snapshot.arp_mode) arp_set_mode(&arp, arp.mode, arp_output);
    else change_arp_mode(snapshot.arp_mode);
    set.oct = snapshot.octave;
    /* The channel byte stays in the on-disk layout for older builds. GLO owns recall-time MIDI settings. */
    fx.on = snapshot.delay_on; fx.division = snapshot.division;
    fx.feedback = snapshot.feedback; fx.mix = snapshot.delay_mix;
    hall_settings.on = snapshot.hall_on; hall_settings.decay = snapshot.hall_decay;
    hall_settings.damping = snapshot.damping; hall_settings.mix = snapshot.hall_mix;
    hall_configure(hall_settings.on, hall_settings.decay, hall_settings.damping, hall_settings.mix);
    return 0;
}

#define SNAPSHOT_HOLD_MS 300u
#define SNAPSHOT_MESSAGE_MS 500u
static uint8_t snapshot_held, snapshot_flashing;
static uint32_t snapshot_pressed_at, snapshot_message_at;
static const char *snapshot_message;

/* Returns 1 only when recall changed the current setup. */
static int snapshot_button(uint32_t pressed, uint32_t released)
{
    const uint32_t button = 1u << BTN_SAVE;
    if (pressed & button) {
        snapshot_pressed_at = fm1_ms;
        snapshot_held = 1;
    }
    if (!snapshot_held) return 0;
    if (fm1_ms - snapshot_pressed_at >= SNAPSHOT_HOLD_MS) {
        snapshot_held = 0;
        int recalled = snapshot_bank >= 0 && snapshot_recall() == 0;
        snapshot_message_at = fm1_ms;
        if (!recalled) {
            snapshot_message = snapshot_bank < 0 ? "NO SNAPSHOT" : "RECALL FAILED";
            return 0;
        }
        snapshot_flashing = 0;
        snapshot_message = "RECALL";
        return 1;
    }
    if (released & button) {
        snapshot_held = 0;
        int saved = snapshot_save() == 0;
        snapshot_message_at = fm1_ms;
        if (!saved) {
            snapshot_message = "SAVE FAILED";
        } else {
            snapshot_flashing = 1;
            snapshot_message = "SNAPSHOT";
        }
    }
    return 0;
}
