/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include "arp.h"

static struct { uint32_t time; uint8_t pitch, velocity; } events[24];
static uint32_t now, count;

static void collect(uint8_t pitch, uint8_t velocity)
{
    assert(count < sizeof events / sizeof events[0]);
    events[count].time = now;
    events[count].pitch = pitch;
    events[count++].velocity = velocity;
}

static arp_t initial_arp(void)
{
    count = now = 0;
    return (arp_t){.rate = ARP_EIGHTH, .gate = 60, .bpm = 120, .spread_ms = 20,
                   .active_note = ARP_NO_NOTE, .last_note = ARP_NO_NOTE};
}

static void expect(uint32_t index, uint32_t time, uint8_t pitch, uint8_t velocity)
{
    assert(index < count && events[index].time == time &&
           events[index].pitch == pitch && events[index].velocity == velocity);
}

static void check_up_down(void)
{
    arp_t arp = initial_arp();
    arp_set_mode(&arp, ARP_UP_DOWN, collect);
    arp_hold(&arp, 64, 90, 0);
    arp_hold(&arp, 67, 80, 0);
    arp_hold(&arp, 60, 100, 0);
    for (now = 0; now < 1001; now++) arp_tick(&arp, now, 0, 0, collect);
    expect(0, 0, 60, 100);
    expect(1, 150, 60, 0);
    expect(2, 250, 64, 90);
    expect(4, 500, 67, 80);
    expect(6, 750, 64, 90);
    expect(8, 1000, 60, 100);

    arp_release(&arp, 60, collect);
    arp_release(&arp, 64, collect);
    arp_release(&arp, 67, collect);
    arp_tick(&arp, now, 0, 0, collect);
    expect(9, 1001, 60, 0);
}

static void check_midi_clock(void)
{
    arp_t arp = initial_arp();
    arp_set_mode(&arp, ARP_DOWN, collect);
    arp_hold(&arp, 60, 100, 0);
    arp_hold(&arp, 64, 90, 0);
    for (uint32_t clock = 0; clock <= 12; clock++) {
        now = clock * 25;
        arp_tick(&arp, now, 600, clock, collect);
    }
    assert(count == 1);
    expect(0, 300, 64, 90);

    /* Time passing without a clock pulse must not start another note. */
    for (now = 301; now < 600; now++) arp_tick(&arp, now, 600, 12, collect);
    assert(count == 2);
    expect(1, 480, 64, 0);
    arp_tick(&arp, now, 600, 24, collect);
    expect(2, 600, 60, 100);
}

static void check_random_half_notes(void)
{
    arp_t arp = initial_arp();
    arp.rate = ARP_HALF;
    arp_set_mode(&arp, ARP_RANDOM, collect);
    arp_hold(&arp, 60, 100, 0);
    arp_hold(&arp, 64, 90, 0);
    arp_hold(&arp, 67, 80, 0);

    uint32_t seen = 0;
    for (uint32_t step = 0; step < 10; step++) {
        now = step * 1000;
        arp_tick(&arp, now, 0, 0, collect);
        assert(count == step * 2 + 1);
        uint32_t pitch = events[count - 1].pitch;
        assert(pitch == 60 || pitch == 64 || pitch == 67);
        expect(count - 1, now, pitch, arp.velocity[pitch]);
        seen |= 1u << (pitch - 60);
        for (uint32_t offset = 1; offset < 1000; offset++) {
            now = step * 1000 + offset;
            arp_tick(&arp, now, 0, 0, collect);
        }
        expect(count - 1, step * 1000 + 600, pitch, 0);
    }
    assert(seen == ((1u << 0) | (1u << 4) | (1u << 7)));
    assert(count == 20);

    arp = initial_arp();
    arp.rate = ARP_HALF;
    arp_set_mode(&arp, ARP_RANDOM, collect);
    arp_hold(&arp, 60, 100, 0);
    for (uint32_t clock = 0; clock < 48; clock++) {
        now = clock * 25;
        arp_tick(&arp, now, 600, clock, collect);
    }
    assert(count == 0);
    now = 1200;
    arp_tick(&arp, now, 600, 48, collect);
    expect(0, 1200, 60, 100);
}

static void check_strum(void)
{
    arp_t arp = initial_arp();
    arp_set_mode(&arp, ARP_STRUM_DOWN, collect);
    arp_hold(&arp, 60, 100, 0);
    arp_hold(&arp, 67, 90, 0);
    arp_hold(&arp, 64, 80, 0);
    arp_hold(&arp, 64, 70, 0);
    for (now = 0; now < 200; now++) arp_tick(&arp, now, 0, 0, collect);
    assert(count == 3);
    expect(0, 24, 67, 90);
    expect(1, 44, 64, 70);
    expect(2, 64, 60, 100);
    arp_release(&arp, 64, collect);
    assert(count == 3);
    arp_release(&arp, 64, collect);
    expect(3, 200, 64, 0);
    arp_set_mode(&arp, ARP_OFF, collect);
    expect(4, 200, 60, 0);
    expect(5, 200, 67, 0);
}

int main(void)
{
    check_up_down();
    check_midi_clock();
    check_random_half_notes();
    check_strum();
    return 0;
}
