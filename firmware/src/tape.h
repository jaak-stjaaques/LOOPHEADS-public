/* SPDX-License-Identifier: GPL-3.0-only */
/* Note-loop timing and seam handling adapted from LOOPHEAD v0.3.0.
 * Original tape engine copyright (C) 2026 Kerem Kilic (Ellic Studio). */
#define TAPE_MAX_NOTES 64u
#define TAPE_MIN_MS 1000
#define TAPE_MAX_MS 30000
#define TAPE_LEAK_END 2520u /* divisible by each pass count from 2 through 10 */
#define TAPE_GAIN_ONE 32768u

enum { TAPE_KEEP, TAPE_STRETCH };

typedef struct {
    uint16_t start_ms, end_ms, reference_ms; /* captured timing, with end zero until release */
    uint16_t play_start_ms, play_end_ms;     /* timing at the current length and mode */
    uint16_t leak_loss;
    uint8_t pitch, velocity, owner, active;
    uint8_t leak_seen_this_pass;
} tape_note_t;

typedef struct {
    tape_note_t notes[TAPE_MAX_NOTES];
    uint16_t length_ms, position_ms;
    uint8_t count, playing, recording, mode, leak_passes;
    uint8_t playing_pitch[128];
} tape_t;

/* Nonscaled velocity and a separate output gain. Zero velocity closes a note. */
typedef void (*tape_event_fn)(const tape_t *tape, uint8_t pitch, uint8_t velocity, uint16_t gain);

static void tape_adjust_leak(tape_t *tape, int32_t steps)
{
    /* Clockwise selects progressively shorter lifetimes after Off. */
    int32_t option = (tape->leak_passes ? 11 - tape->leak_passes : 0) + steps;
    option = option < 0 ? 0 : option > 9 ? 9 : option;
    tape->leak_passes = (uint8_t)(option ? 11 - option : 0);
}

static void tape_close_note(tape_note_t *note, uint16_t end_ms)
{
    /* A recording can only close at the length it was captured at. */
    note->end_ms = end_ms > note->start_ms ? end_ms : note->start_ms + 1u;
    note->play_end_ms = note->end_ms;
}

static void tape_capture_off(tape_t *tape, uint8_t owner, uint8_t pitch)
{
    if (!tape->recording) return;
    for (uint32_t i = 0; i < tape->count; i++) {
        tape_note_t *note = &tape->notes[i];
        if (!note->end_ms && note->owner == owner && note->pitch == pitch)
            tape_close_note(note, tape->position_ms);
    }
}

static void tape_capture_on(tape_t *tape, uint8_t owner, uint8_t pitch, uint8_t velocity)
{
    if (!tape->recording) return;
    tape_capture_off(tape, owner, pitch);
    if (tape->count == TAPE_MAX_NOTES) return;
    tape->notes[tape->count++] = (tape_note_t){
        .start_ms = tape->position_ms, .reference_ms = tape->length_ms,
        .play_start_ms = tape->position_ms, .pitch = pitch, .velocity = velocity, .owner = owner,
        .leak_seen_this_pass = 1  /* the live performance already sounded on this pass */
    };
}

static void tape_close_captures(tape_t *tape, uint16_t end_ms)
{
    for (uint32_t i = 0; i < tape->count; i++) {
        tape_note_t *note = &tape->notes[i];
        if (!note->end_ms)
            tape_close_note(note, end_ms);
    }
}

static void tape_release_note(tape_t *tape, tape_note_t *note, tape_event_fn event)
{
    note->active = 0;
    if (--tape->playing_pitch[note->pitch] == 0)
        event(tape, note->pitch, 0, 0);
}

static uint32_t tape_scale_ms(uint32_t ms, uint16_t length, uint16_t reference)
{
    return (ms * length + reference / 2u) / reference;
}

static void tape_set_timing(tape_t *tape, uint16_t length, uint8_t mode, tape_event_fn event)
{
    if (tape->recording || length < TAPE_MIN_MS || length > TAPE_MAX_MS ||
        (length == tape->length_ms && mode == tape->mode)) return;

    uint8_t wrapped = tape->position_ms >= length;
    if (wrapped) tape->position_ms %= length;
    tape->length_ms = length;
    tape->mode = mode;

    for (uint32_t i = 0; i < tape->count; i++) {
        tape_note_t *note = &tape->notes[i];
        uint32_t start = note->start_ms;
        uint32_t duration = note->end_ms - note->start_ms;
        if (mode == TAPE_STRETCH) {
            start = tape_scale_ms(start, length, note->reference_ms);
            if (start >= length) start = length - 1u;
            duration = tape_scale_ms(duration, length, note->reference_ms);
            if (!duration) duration = 1;
        }
        uint32_t end = start + duration;
        note->play_start_ms = (uint16_t)start;
        note->play_end_ms = (uint16_t)(end < length ? end : length);

        /* Edits only keep already sounding notes whose new interval still covers the playhead. */
        if (note->active && (wrapped || tape->position_ms < start ||
                             tape->position_ms >= note->play_end_ms))
            tape_release_note(tape, note, event);
    }
}

static uint16_t tape_leak_gain(uint16_t loss)
{
    static const uint16_t gain[] = {
        TAPE_GAIN_ONE, 23198, 16423, 11627, 8231, 5827, 4125, 2920, 2068, 1464, 1036
    };
    if (!loss) return TAPE_GAIN_ONE;
    if (loss >= TAPE_LEAK_END) return 0;
    /* Fade by up to 30 dB before the final pass erases the note. */
    uint32_t position = (uint32_t)loss * 10u;
    uint32_t i = position / TAPE_LEAK_END;
    uint32_t blend = position % TAPE_LEAK_END;
    return (uint16_t)(gain[i] - (uint32_t)(gain[i] - gain[i + 1]) * blend / TAPE_LEAK_END);
}

static void tape_dispatch(tape_t *tape, tape_event_fn event, uint8_t crossing, uint8_t erase)
{
    /* At a shared timestamp, release the old sounds before starting new ones. */
    for (uint32_t i = 0; i < tape->count; i++) {
        tape_note_t *note = &tape->notes[i];
        if (note->active && note->play_end_ms == tape->position_ms)
            tape_release_note(tape, note, event);
    }
    uint16_t leak_step = crossing && tape->leak_passes ? TAPE_LEAK_END / tape->leak_passes : 0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < tape->count; i++) {
        tape_note_t *note = &tape->notes[i];
        if (note->end_ms && !note->active && note->play_start_ms < tape->length_ms &&
            note->play_start_ms == tape->position_ms) {
            if (erase) continue;
            if (!note->leak_seen_this_pass) {
                note->leak_seen_this_pass = 1;
                note->leak_loss += leak_step;
            }
            if (note->leak_loss >= TAPE_LEAK_END) continue;
            note->active = 1;
            tape->playing_pitch[note->pitch]++;
            event(tape, note->pitch, note->velocity, tape_leak_gain(note->leak_loss));
        }
        /* Erased and expired notes have no active voice. Keep the other records in order. */
        if (kept != i) tape->notes[kept] = *note;
        kept++;
    }
    tape->count = (uint8_t)kept;
}

static void tape_tick_with_erase(tape_t *tape, uint32_t elapsed_ms, tape_event_fn event,
                                 uint32_t erase_after_ms)
{
    if (!tape->playing) return;
    uint32_t total_ms = elapsed_ms;
    while (elapsed_ms) {
        uint16_t next = tape->length_ms;
        for (uint32_t i = 0; i < tape->count; i++) {
            const tape_note_t *note = &tape->notes[i];
            if (!note->end_ms) continue;
            uint16_t at = note->active ? note->play_end_ms : note->play_start_ms;
            if (at > tape->position_ms && at < next) next = at;
        }
        uint32_t distance = (uint32_t)next - tape->position_ms;
        if (elapsed_ms < distance) {
            tape->position_ms += (uint16_t)elapsed_ms;
            return;
        }

        elapsed_ms -= distance;
        tape->position_ms = next;
        if (next == tape->length_ms) {
            tape_close_captures(tape, next);
            tape_dispatch(tape, event, 0, 0);
            /* Only completing a loop opens a new pass. Editing or rewinding can revisit
             * an onset but must not use up another pass of that note's lifetime. */
            for (uint32_t i = 0; i < tape->count; i++)
                tape->notes[i].leak_seen_this_pass = 0;
            tape->position_ms = 0;
        }
        uint8_t erase = !tape->recording && erase_after_ms != UINT32_MAX &&
                        total_ms - elapsed_ms >= erase_after_ms;
        tape_dispatch(tape, event, 1, erase);
    }
}

static void tape_tick(tape_t *tape, uint32_t elapsed_ms, tape_event_fn event)
{
    tape_tick_with_erase(tape, elapsed_ms, event, UINT32_MAX);
}

static void tape_play(tape_t *tape, tape_event_fn event)
{
    if (tape->playing) return;
    tape->playing = 1;
    tape_dispatch(tape, event, 0, 0);
}

static void tape_end_recording(tape_t *tape)
{
    tape_close_captures(tape, tape->position_ms);
    tape->recording = 0;
}

static void tape_stop(tape_t *tape, tape_event_fn event)
{
    tape_end_recording(tape);
    for (uint32_t i = 0; i < tape->count; i++) {
        tape_note_t *note = &tape->notes[i];
        if (note->active) tape_release_note(tape, note, event);
    }
    tape->playing = 0;
    tape->position_ms = 0;
}
