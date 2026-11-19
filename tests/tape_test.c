/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include "tape.h"

static struct { uint8_t pitch, velocity; uint16_t gain; } events[16];
static uint32_t event_count;

static void collect(const tape_t *tape, uint8_t pitch, uint8_t velocity, uint16_t gain)
{
    (void)tape;
    assert(event_count < sizeof events / sizeof events[0]);
    events[event_count].pitch = pitch;
    events[event_count].velocity = velocity;
    events[event_count++].gain = gain;
}

static void check_overdub_and_stop(void)
{
    tape_t tape = {.length_ms = 8000};
    event_count = 0;
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_tick(&tape, 100, collect);
    tape_capture_on(&tape, 1, 60, 90);
    tape_tick(&tape, 30, collect);
    tape_capture_on(&tape, 2, 60, 100);
    tape_tick(&tape, 20, collect);
    tape_capture_off(&tape, 1, 60);
    tape_tick(&tape, 50, collect);
    tape_end_recording(&tape);

    assert(tape.count == 2 && tape.notes[0].end_ms == 150 && tape.notes[1].end_ms == 200);
    assert(tape.playing && event_count == 0);
    tape_tick(&tape, 7900, collect);
    assert(event_count == 1 && events[0].velocity == 90);
    tape_tick(&tape, 50, collect);
    assert(event_count == 2 && events[1].velocity == 100);
    tape_stop(&tape, collect);
    assert(event_count == 3 && events[2].pitch == 60 && !events[2].velocity);
    assert(!tape.playing && !tape.position_ms && !tape.playing_pitch[60]);

    tape_play(&tape, collect);
    tape_tick(&tape, 200, collect);
    assert(event_count == 6 && events[3].velocity == 90 &&
           events[4].velocity == 100 && !events[5].velocity);
}

static void check_wrap_and_capacity(void)
{
    tape_t tape = {.length_ms = 8000};
    event_count = 0;
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_tick(&tape, 7980, collect);
    tape_capture_on(&tape, 1, 64, 80);
    tape_tick(&tape, 20, collect);
    assert(tape.position_ms == 0 && tape.notes[0].end_ms == 8000);
    tape_capture_off(&tape, 1, 64);
    tape_end_recording(&tape);
    tape_tick(&tape, 8000, collect);
    assert(event_count == 2 && events[0].velocity == 80 && !events[1].velocity);

    tape.recording = 1;
    for (uint32_t pitch = 0; pitch < TAPE_MAX_NOTES; pitch++)
        tape_capture_on(&tape, 1, (uint8_t)pitch, 90);
    assert(tape.count == TAPE_MAX_NOTES && tape.notes[0].pitch == 64);
    assert(tape.notes[TAPE_MAX_NOTES - 1].pitch == 62);
    tape_end_recording(&tape);
}

static void check_length_modes(void)
{
    tape_t tape = {.length_ms = 8000};
    event_count = 0;
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_tick(&tape, 2000, collect);
    tape_capture_on(&tape, 1, 60, 100);
    tape_tick(&tape, 500, collect);
    tape_capture_off(&tape, 1, 60);
    tape_tick(&tape, 5000, collect);
    tape_capture_on(&tape, 1, 64, 90);
    tape_tick(&tape, 400, collect);
    tape_capture_off(&tape, 1, 64);
    tape_set_timing(&tape, 4000, TAPE_STRETCH, collect);
    assert(tape.length_ms == 8000 && tape.mode == TAPE_KEEP);
    tape_stop(&tape, collect);

    tape_set_timing(&tape, 4000, TAPE_KEEP, collect);
    assert(tape.count == 2);
    assert(tape.notes[0].play_start_ms == 2000 && tape.notes[0].play_end_ms == 2500);
    assert(tape.notes[1].play_start_ms >= tape.length_ms);
    tape_play(&tape, collect);
    tape_tick(&tape, 4000, collect);
    assert(event_count == 2 && events[0].pitch == 60 && !events[1].velocity);
    tape_stop(&tape, collect);
    event_count = 0;

    tape_set_timing(&tape, 4000, TAPE_STRETCH, collect);
    assert(tape.notes[0].play_start_ms == 1000 && tape.notes[0].play_end_ms == 1250);
    assert(tape.notes[1].play_start_ms == 3750 && tape.notes[1].play_end_ms == 3950);
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_tick(&tape, 200, collect);
    tape_capture_on(&tape, 1, 67, 100);
    tape_tick(&tape, 200, collect);
    tape_capture_off(&tape, 1, 67);
    tape_stop(&tape, collect);

    tape_set_timing(&tape, 12000, TAPE_STRETCH, collect);
    assert(tape.notes[0].play_start_ms == 3000 && tape.notes[0].play_end_ms == 3750);
    assert(tape.notes[2].play_start_ms == 600 && tape.notes[2].play_end_ms == 1200);
    tape_set_timing(&tape, 8000, TAPE_KEEP, collect);
    assert(tape.notes[1].play_start_ms == 7500 && tape.notes[1].play_end_ms == 7900);
}

static void check_edit_at_loop_boundary(void)
{
    tape_t tape = {.length_ms = 30000};
    event_count = 0;
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_tick(&tape, 29999, collect);
    tape_capture_on(&tape, 1, 60, 100);
    tape_tick(&tape, 1, collect);
    tape_stop(&tape, collect);
    tape_set_timing(&tape, 1000, TAPE_STRETCH, collect);
    assert(tape.notes[0].play_start_ms == 999 && tape.notes[0].play_end_ms == 1000);
    tape_play(&tape, collect);
    tape_tick(&tape, 999, collect);
    assert(event_count == 1 && events[0].velocity == 100);
    tape_set_timing(&tape, 1000, TAPE_KEEP, collect);
    assert(event_count == 2 && !events[1].velocity && !tape.notes[0].active);
    tape_tick(&tape, 1001, collect);
    assert(event_count == 2); /* a hidden note cannot trigger at the seam */
}

static void check_memory_leak(void)
{
    tape_t tape = {.length_ms = 1000};
    event_count = 0;
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_capture_on(&tape, 1, 60, 100);
    tape_tick(&tape, 100, collect);
    tape_capture_off(&tape, 1, 60);
    tape_stop(&tape, collect);

    tape.leak_passes = 3;
    tape_play(&tape, collect);
    assert(event_count == 1 && events[0].gain == TAPE_GAIN_ONE);
    tape_tick(&tape, 1000, collect);
    uint16_t first_gain = events[2].gain;
    assert(event_count == 3 && first_gain < TAPE_GAIN_ONE && events[2].velocity == 100);
    assert(tape.notes[0].leak_loss == TAPE_LEAK_END / 3);

    tape.leak_passes = 0;
    tape_tick(&tape, 1000, collect);
    assert(event_count == 5 && events[4].gain == first_gain && events[4].velocity == 100);

    tape.leak_passes = 3;
    tape_tick(&tape, 1000, collect);
    assert(event_count == 7 && events[6].gain < first_gain && events[6].velocity == 100);
    tape_tick(&tape, 1000, collect);
    assert(event_count == 8 && !events[7].velocity);
    assert(tape.count == 0 && !tape.playing_pitch[60]);
}

static void check_hidden_note_does_not_leak(void)
{
    tape_t tape = {.length_ms = 2000};
    event_count = 0;
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_tick(&tape, 1500, collect);
    tape_capture_on(&tape, 1, 60, 100);
    tape_tick(&tape, 100, collect);
    tape_stop(&tape, collect);
    tape.leak_passes = 2;
    tape_set_timing(&tape, 1000, TAPE_KEEP, collect);
    tape_play(&tape, collect);
    tape_tick(&tape, 9000, collect);
    assert(event_count == 0 && tape.count == 1 && !tape.notes[0].leak_loss);
    tape_set_timing(&tape, 2000, TAPE_KEEP, collect);
    tape_tick(&tape, 1500, collect);
    assert(event_count == 1 && events[0].velocity == 100 && events[0].gain < TAPE_GAIN_ONE);
}

static void check_leak_knob_order(void)
{
    tape_t tape = {0};
    tape_adjust_leak(&tape, -1);
    assert(tape.leak_passes == 0);
    for (uint8_t passes = 10; passes >= 2; passes--) {
        tape_adjust_leak(&tape, 1);
        assert(tape.leak_passes == passes);
    }
    tape_adjust_leak(&tape, 1);
    assert(tape.leak_passes == 2);
    tape_adjust_leak(&tape, -8);
    assert(tape.leak_passes == 10);
    tape_adjust_leak(&tape, -1);
    assert(tape.leak_passes == 0);
}

static void check_late_overdub_lifetime(void)
{
    tape_t tape = {.length_ms = 1000, .leak_passes = 10};
    event_count = 0;
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_tick(&tape, 100, collect);
    tape_capture_on(&tape, 1, 60, 100);
    tape_tick(&tape, 70, collect);
    tape_end_recording(&tape);
    for (uint32_t pass = 0; pass < 4; pass++) {
        event_count = 0;
        tape_tick(&tape, 1000, collect);
    }
    assert(tape.notes[0].leak_loss == 4 * (TAPE_LEAK_END / 10));
    tape.recording = 1;
    tape_capture_on(&tape, 1, 72, 110);
    tape_tick(&tape, 70, collect);
    tape_end_recording(&tape);
    assert(tape.count == 2 && !tape.notes[1].leak_loss);

    for (uint32_t pass = 1; pass <= 10; pass++) {
        event_count = 0;
        tape_tick(&tape, 1000, collect);
        if (pass < 6) {
            assert(tape.count == 2);
        } else if (pass < 10) {
            assert(tape.count == 1 && tape.notes[0].pitch == 72);
        } else {
            assert(tape.count == 0);
        }
        if (pass < 10) {
            const tape_note_t *overdub = &tape.notes[tape.count - 1u];
            assert(overdub->velocity == 110 && overdub->leak_loss == pass * (TAPE_LEAK_END / 10));
        }
    }
}

static void check_edit_and_restart_do_not_age_twice(void)
{
    tape_t tape = {.length_ms = 4000, .leak_passes = 10};
    event_count = 0;
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_tick(&tape, 500, collect);
    tape_capture_on(&tape, 1, 60, 100);
    tape_tick(&tape, 100, collect);
    tape_end_recording(&tape);
    tape_tick(&tape, 4000, collect);
    uint16_t loss = tape.notes[0].leak_loss;
    assert(loss == TAPE_LEAK_END / 10);

    /* Stretch moves the onset from 500 ms to 1000 ms within this same pass. */
    tape_set_timing(&tape, 8000, TAPE_STRETCH, collect);
    tape_tick(&tape, 1000, collect);
    assert(tape.notes[0].leak_loss == loss);
    tape_stop(&tape, collect);
    tape_play(&tape, collect);
    tape_tick(&tape, 1100, collect);
    assert(tape.notes[0].leak_loss == loss);
}

static void check_erase_crossings(void)
{
    tape_t tape = {.length_ms = 1000};
    event_count = 0;
    tape_play(&tape, collect);
    tape.recording = 1;
    tape_capture_on(&tape, 1, 60, 100);
    tape_tick(&tape, 10, collect);
    tape_capture_off(&tape, 1, 60);
    tape_tick(&tape, 890, collect);
    tape_capture_on(&tape, 1, 62, 100);
    tape_tick(&tape, 10, collect);
    tape_end_recording(&tape);
    tape_stop(&tape, collect);
    tape_play(&tape, collect);
    tape_tick_with_erase(&tape, 1020, collect, 950);
    assert(tape.count == 1 && tape.notes[0].pitch == 62);
    assert(!tape.playing_pitch[60]);
    assert(event_count == 4 && events[2].pitch == 62 && events[3].pitch == 62);
    tape_tick(&tape, 900, collect);
    assert(event_count == 6 && events[4].pitch == 62 && events[5].pitch == 62);
}

int main(void)
{
    check_overdub_and_stop();
    check_wrap_and_capacity();
    check_length_modes();
    check_edit_at_loop_boundary();
    check_memory_leak();
    check_hidden_note_does_not_leak();
    check_leak_knob_order();
    check_late_overdub_lifetime();
    check_erase_crossings();
    check_edit_and_restart_do_not_age_twice();
    return 0;
}
