/* SPDX-License-Identifier: GPL-3.0-only */
/* Snapshots store these IDs. The menu order is independent of their values. */
enum { ARP_OFF, ARP_UP, ARP_DOWN, ARP_UP_DOWN, ARP_STRUM_UP, ARP_STRUM_DOWN,
       ARP_RANDOM, ARP_MODES };
enum { ARP_QUARTER, ARP_EIGHTH, ARP_EIGHTH_TRIPLET, ARP_SIXTEENTH,
       ARP_SIXTEENTH_TRIPLET, ARP_THIRTYSECOND, ARP_HALF, ARP_RATES };
#define ARP_NO_NOTE 255u
#define STRUM_GATHER_MS 24u

static const char *const arp_mode_names[ARP_MODES] = {
    "OFF", "UP", "DOWN", "UP / DOWN", "STRUM UP", "STRUM DOWN", "RANDOM"
};
static const char *const arp_rate_names[ARP_RATES] = {
    "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/2"
};
static const uint8_t arp_rate_clocks[ARP_RATES] = {24, 12, 8, 6, 4, 3, 48};

typedef void (*arp_event_fn)(uint8_t pitch, uint8_t velocity);

typedef struct {
    uint8_t refs[128], velocity[128], strummed[128];
    uint8_t mode, rate, gate, bpm, spread_ms;
    uint8_t held_count, pending, active_note, last_note, descending, running, external;
    uint32_t next_ms, release_ms, clock_slot, random;
} arp_t;

static uint8_t arp_is_strum(const arp_t *arp)
{
    return arp->mode == ARP_STRUM_UP || arp->mode == ARP_STRUM_DOWN;
}

static int arp_due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void arp_stop_note(arp_t *arp, arp_event_fn event)
{
    if (arp->active_note == ARP_NO_NOTE) return;
    event(arp->active_note, 0);
    arp->active_note = ARP_NO_NOTE;
}

static void arp_set_mode(arp_t *arp, uint8_t mode, arp_event_fn event)
{
    arp_stop_note(arp, event);
    if (arp_is_strum(arp)) {
        for (uint32_t i = 0; i < 128; i++) {
            if (arp->strummed[i]) event((uint8_t)i, 0);
            arp->strummed[i] = 0;
        }
    }
    arp->mode = mode;
    arp->pending = arp_is_strum(arp) ? arp->held_count : 0;
    arp->last_note = ARP_NO_NOTE;
    arp->descending = arp->running = 0;
}

static void arp_hold(arp_t *arp, uint8_t note, uint8_t velocity, uint8_t already_held)
{
    arp->velocity[note] = velocity;
    if (already_held) return;
    if (++arp->refs[note] != 1) return;
    arp->held_count++;
    if (arp_is_strum(arp)) arp->pending++;
}

static void arp_release(arp_t *arp, uint8_t note, arp_event_fn event)
{
    if (--arp->refs[note]) return;
    arp->held_count--;
    if (!arp_is_strum(arp)) return;
    if (arp->strummed[note]) {
        event(note, 0);
        arp->strummed[note] = 0;
    } else {
        arp->pending--;
    }
    if (!arp->pending) arp->running = 0;
}

static int32_t arp_find(const arp_t *arp, int32_t start, int32_t direction)
{
    for (int32_t note = start; note >= 0 && note < 128; note += direction)
        if (arp->refs[note]) return note;
    return -1;
}

static uint8_t arp_random_pitch(arp_t *arp, uint32_t now)
{
    uint32_t random = arp->random ? arp->random : (now | 1u);
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    arp->random = random;

    uint32_t index = random % arp->held_count;
    for (uint32_t pitch = 0; pitch < 128; pitch++)
        if (arp->refs[pitch] && index-- == 0) return (uint8_t)pitch;
    return ARP_NO_NOTE;
}

static uint8_t arp_next_pitch(arp_t *arp, uint32_t now)
{
    if (arp->mode == ARP_RANDOM) return arp_random_pitch(arp, now);
    int32_t direction = arp->mode == ARP_DOWN || arp->descending ? -1 : 1;
    int32_t edge = direction > 0 ? 0 : 127;
    int32_t pitch = arp_find(arp, arp->last_note == ARP_NO_NOTE ? edge : arp->last_note + direction, direction);
    if (pitch < 0 && arp->mode == ARP_UP_DOWN) {
        arp->descending = !arp->descending;
        direction = -direction;
        pitch = arp_find(arp, arp->last_note + direction, direction);
    }
    if (pitch < 0) pitch = arp_find(arp, direction > 0 ? 0 : 127, direction);
    return (uint8_t)pitch;
}

static void arp_strum_tick(arp_t *arp, uint32_t now, arp_event_fn event)
{
    if (!arp->pending) {
        arp->running = 0;
        return;
    }
    if (!arp->running) {
        arp->running = 1;
        arp->next_ms = now + STRUM_GATHER_MS;
        return;
    }
    if (!arp_due(now, arp->next_ms)) return;
    int32_t direction = arp->mode == ARP_STRUM_UP ? 1 : -1;
    for (int32_t note = direction > 0 ? 0 : 127; note >= 0 && note < 128; note += direction) {
        if (!arp->refs[note] || arp->strummed[note]) continue;
        event((uint8_t)note, arp->velocity[note]);
        arp->strummed[note] = 1;
        arp->pending--;
        arp->next_ms = now + arp->spread_ms;
        break;
    }
}

static void arp_tick(arp_t *arp, uint32_t now, uint32_t external_quarter_ms,
                     uint32_t clocks, arp_event_fn event)
{
    if (arp->mode == ARP_OFF) return;
    if (arp_is_strum(arp)) {
        arp_strum_tick(arp, now, event);
        return;
    }
    if (!arp->held_count || arp_due(now, arp->release_ms)) arp_stop_note(arp, event);
    if (!arp->held_count) {
        arp->running = 0;
        arp->last_note = ARP_NO_NOTE;
        arp->descending = 0;
        return;
    }

    uint8_t external = external_quarter_ms != 0;
    uint32_t clocks_per_step = arp_rate_clocks[arp->rate];
    uint32_t slot = external ? clocks / clocks_per_step : 0;
    if (!arp->running || arp->external != external) {
        arp->running = 1;
        arp->external = external;
        arp->clock_slot = slot;
        arp->next_ms = now;
    }
    if (external ? slot == arp->clock_slot : !arp_due(now, arp->next_ms)) return;
    arp->clock_slot = slot;

    uint32_t quarter = external ? external_quarter_ms : 60000u / arp->bpm;
    uint32_t interval = (quarter * clocks_per_step + 12u) / 24u;
    uint8_t note = arp_next_pitch(arp, now);
    arp_stop_note(arp, event);
    event(note, arp->velocity[note]);
    arp->last_note = arp->active_note = note;
    arp->release_ms = now + interval * arp->gate / 100u;
    arp->next_ms = now + interval;
}
