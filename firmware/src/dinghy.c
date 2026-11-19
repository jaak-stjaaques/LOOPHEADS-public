/* SPDX-License-Identifier: GPL-3.0-only */
/* LOOPHEADS instrument app, adapted from Dinghy's keyboard and MIDI demo. */
#define APP_NAME "LOOPHEADS"
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif

#define TAPE_COUNT 4u
#include "pluck.h"
#include "tape.h"
#include "scale.h"
#include "arp.h"
#include "effects.h"

static int32_t output_level(int32_t sample)
{
    /* Raise quiet notes by about 9.5 dB and soften peaks before the master volume. */
    int32_t magnitude = (sample < 0 ? -sample : sample) * 3;
    if (magnitude > 20000) {
        int32_t excess = magnitude - 20000;
        magnitude = 20000 + 12000 * excess / (12000 + excess);
    }
    return sample < 0 ? -magnitude : magnitude;
}

static void app_audio(int32_t *out, uint32_t frames)
{
    pluck_mix(out, frames);
    effects_process(out, frames);
    for (uint32_t i = 0; i < 2u * frames; i++) out[i] = output_level(out[i]);
}

#if BOARD_SMK37
#define KEY_OWNER_OFFSET SMK_KEY_F3
#else
#define KEY_OWNER_OFFSET 0u
#endif

enum { OWNER_KEYS = 0, OWNER_PADS = 37, OWNER_USB = 53, OWNER_TRS = OWNER_USB + 16, OWNER_TAPE = OWNER_TRS + 16,
       OWNER_ARP = OWNER_TAPE + TAPE_COUNT };
enum { PAGE_HOME, PAGE_EDIT, PAGE_TAPE, PAGE_LEAK, PAGE_ARP, PAGE_FX, PAGE_HALL, PAGE_ENV, PAGE_LFO, PAGE_SCALE, PAGE_PAN, PAGE_MUTE, PAGE_GLO };
static tape_t tapes[TAPE_COUNT] = {
    {.length_ms = 8000}, {.length_ms = 13000}, {.length_ms = 5000}, {.length_ms = 21000}
};
static uint32_t tape_ms;
static uint8_t armed_tape;

#define ERASE_HOLD_MS 300u
static uint8_t erase_pending, erase_tape;
static uint32_t erase_started;

static uint8_t erase_held(void)
{
    return erase_pending && (fm1_in.buttons & (1u << BTN_EDIT)) &&
           armed_tape == erase_tape && tapes[erase_tape].playing && !tapes[erase_tape].recording;
}

#define NO_TRANSPOSE_ROOT 255u
static uint8_t global_scale, scale_root;
static uint8_t transpose_root = NO_TRANSPOSE_ROOT;

static uint8_t effective_tape_root(void)
{
    return transpose_root == NO_TRANSPOSE_ROOT ? scale_root : transpose_root;
}

static uint8_t tape_sounding_pitch(uint8_t raw)
{
    uint8_t root = effective_tape_root();
    int32_t shift = (int32_t)root - scale_root;
    if (shift > 6) shift -= 12;
    if (shift < -6) shift += 12;
    int32_t moved = raw + shift;
    if (moved < 0) moved += 12;
    if (moved > 127) moved -= 12;
    return scale_note(global_scale, root, (uint8_t)moved);
}
/* Remember each raw note's sounding pitch until release, even if the scale changes. */
static uint8_t sounding_pitch[OWNER_ARP + 1][128];

static void sound_stop(uint8_t owner, uint8_t raw)
{
    uint8_t *notes = sounding_pitch[owner];
    uint8_t pitch = notes[raw];
    if (!pitch) return;
    notes[raw] = 0;
    for (uint32_t i = 0; i < 128; i++)
        if (notes[i] == pitch) return;
    pluck_stop(owner, pitch - 1u);
}

static void sound_start(uint8_t owner, uint8_t part, uint8_t raw, uint8_t pitch,
                        uint8_t velocity, uint16_t gain)
{
    if (sounding_pitch[owner][raw] != pitch + 1u) sound_stop(owner, raw);
    sounding_pitch[owner][raw] = pitch + 1u;
    pluck_start(owner, part, pitch, velocity, gain);
}

static void tape_event(const tape_t *tape, uint8_t note, uint8_t velocity, uint16_t gain)
{
    uint8_t part = (uint8_t)(tape - tapes);
    uint8_t owner = OWNER_TAPE + part;
    if (velocity)
        sound_start(owner, part, note, tape_sounding_pitch(note), velocity, gain);
    else
        sound_stop(owner, note);
}

static void retune_playing_tapes(void)
{
    for (uint32_t part = 0; part < TAPE_COUNT; part++) {
        const tape_t *tape = &tapes[part];
        if (!tape->playing) continue;
        uint8_t owner = OWNER_TAPE + part;
        for (uint32_t raw = 0; raw < 128; raw++) {
            if (tape->playing_pitch[raw] && sounding_pitch[owner][raw] != tape_sounding_pitch((uint8_t)raw) + 1u)
                sound_stop(owner, (uint8_t)raw);
        }
        for (uint32_t i = 0; i < tape->count; i++) {
            const tape_note_t *note = &tape->notes[i];
            if (!note->active || sounding_pitch[owner][note->pitch]) continue;
            const tape_note_t *latest = note;
            for (uint32_t j = i + 1; j < tape->count; j++) {
                const tape_note_t *candidate = &tape->notes[j];
                if (candidate->active && candidate->pitch == note->pitch &&
                    candidate->play_start_ms >= latest->play_start_ms)
                    latest = candidate;
            }
            tape_event(tape, latest->pitch, latest->velocity, tape_leak_gain(latest->leak_loss));
        }
    }
}

static void advance_tapes(void)
{
    uint32_t now = fm1_ms;
    uint32_t elapsed = now - tape_ms;
    int32_t until_erase = (int32_t)(erase_started + ERASE_HOLD_MS - tape_ms);
    uint8_t can_erase = erase_held();
    tape_ms = now;
    if (!elapsed) return;
    for (uint32_t i = 0; i < TAPE_COUNT; i++) {
        if (can_erase && i == erase_tape)
            tape_tick_with_erase(&tapes[i], elapsed, tape_event, until_erase > 0 ? (uint32_t)until_erase : 0);
        else
            tape_tick(&tapes[i], elapsed, tape_event);
    }
}

static void tape_select(int32_t step)
{
    uint8_t next = (uint8_t)(((uint32_t)armed_tape + (uint32_t)step) % TAPE_COUNT);
    if (next != armed_tape) {
        if (tapes[armed_tape].recording) tape_end_recording(&tapes[armed_tape]);
        armed_tape = next;
    }
}

static void play_tapes(void)
{
    for (uint32_t i = 0; i < TAPE_COUNT; i++)
        tape_play(&tapes[i], tape_event);
}

static int can_transpose_tapes(void)
{
    for (uint32_t i = 0; i < TAPE_COUNT; i++)
        if (tapes[i].recording) return 0;
    return tapes[armed_tape].playing;
}

static void tape_transport(uint32_t buttons)
{
    tape_t *tape = &tapes[armed_tape];
    if (buttons & (1u << BTN_REC)) {
        if (tape->recording) {
            tape_end_recording(tape);
        } else {
            play_tapes();
            tape->recording = 1;
        }
    } else if (buttons & (1u << BTN_PLAY)) {
        if (tape->recording) {
            tape_end_recording(tape);
        } else if (tape->playing) {
            for (uint32_t i = 0; i < TAPE_COUNT; i++) tape_stop(&tapes[i], tape_event);
        } else {
            play_tapes();
        }
    }
}

static void play(uint8_t owner, uint32_t note, uint32_t velocity)
{
    if (note > 127u || velocity == 0 || velocity > 127u) return;
    uint8_t pitch = scale_note(global_scale, scale_root, (uint8_t)note);
    sound_start(owner, armed_tape, (uint8_t)note, pitch, (uint8_t)velocity, PLUCK_GAIN_ONE);
    tape_capture_on(&tapes[armed_tape], owner, (uint8_t)note, (uint8_t)velocity);
}

static void stop(uint8_t owner, uint8_t note)
{
    sound_stop(owner, note);
    tape_capture_off(&tapes[armed_tape], owner, note);
}

static int quiet(void)
{
    for (uint32_t i = 0; i < PLUCK_VOICES; i++)
        if (pluck_voices[i].active) return 0;
    return 1;
}

#include "loopheads_font.h"
#include "loopheads_boot_logo.h"

#define C_BG RGB(11, 17, 20)
#define C_ACC RGB(117, 222, 213)
#define C_KEY RGB(230, 240, 239)
#define C_OFF RGB(163, 179, 187)
#define C_RULE RGB(51, 69, 75)
#define C_GRAPH RGB(23, 39, 42)
#define UI_LEFT 15u
#define UI_WIDTH 210u
#define UI_FOOTER_Y 218u
#define UI_BATTERY_WIDTH 27u
#define BOOT_LOGO_MS 1200u
#define KEY0 53u                                /* key 0 = F3 */

static void ui_text(uint32_t x, uint32_t y, uint32_t width, const felucca_font_t *font,
                    const char *text, uint16_t color, int align)
{
    draw_text_box_bg(x, y, width, font, text, color, align, C_BG);
}

static void draw_boot_logo(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    uint32_t run = 0, remaining = 0;
    uint16_t color = 0;
    for (uint32_t y = 0; y < BOOT_LOGO_HEIGHT; y += cv_h) {
        cv_begin(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT - y, C_BG);
        uint16_t *pixel = cv_px;
        uint32_t pixels = cv_w * cv_h;
        while (pixels) {
            if (!remaining) {
                remaining = boot_logo_runs[run++];
                color = swap16(boot_logo_runs[run++]);
            }
            uint32_t count = remaining < pixels ? remaining : pixels;
            remaining -= count;
            pixels -= count;
            while (count--) *pixel++ = color;
        }
        cv_blit(BOOT_LOGO_X, BOOT_LOGO_Y + y);
    }
    ui_text(UI_LEFT, 201, UI_WIDTH, &UI_SMALL, "v" APP_VER, C_OFF, 1);
}

static void ui_title(const char *title)
{
    ui_text(UI_LEFT, 13, UI_WIDTH - UI_BATTERY_WIDTH - 8u, &UI_LARGE, title, C_ACC, 0);
}

#if !BOARD_SMK37
static uint8_t read_battery_level(uint32_t now)
{
    static uint32_t last_read;
    static int32_t average;
    static uint8_t level = UINT8_MAX;
    if (now - last_read >= 250u) {
        last_read = now;
        int32_t raw = fm1_adc_read(FM1_ADC_BATT);
        if (raw > 0) {
            average = average ? average + (raw * 16 - average) / 4 : raw * 16;
            /* FM-1 battery-divider thresholds from the earlier LOOPHEAD firmware. */
            level = average >= 591 * 16 ? 3 : average >= 561 * 16 ? 2 :
                    average >= 531 * 16 ? 1 : 0;
        }
    }
    return level;
}

static void draw_battery(uint8_t level)
{
    uint16_t outline = level == UINT8_MAX ? C_RULE : C_OFF;
    cv_begin(UI_BATTERY_WIDTH, 12, C_BG);
    cv_rect(0, 0, 24, 12, outline);
    cv_rect(1, 1, 22, 10, C_BG);
    cv_rect(24, 4, 3, 4, outline);
    if (level != UINT8_MAX)
        for (uint32_t i = 0; i < level; i++)
            cv_rect(3 + i * 6, 3, 5, 6, C_ACC);
    cv_blit(UI_LEFT + UI_WIDTH - UI_BATTERY_WIDTH, 17);
}
#endif

static void ui_footer(const char *hint)
{
    ui_text(UI_LEFT, UI_FOOTER_Y, UI_WIDTH, &UI_SMALL, hint, C_OFF, 1);
}

static void ui_footer_pair(const char *left, const char *right)
{
    ui_text(UI_LEFT, UI_FOOTER_Y, 90, &UI_SMALL, left, C_OFF, 0);
    ui_text(105, UI_FOOTER_Y, 120, &UI_SMALL, right, C_OFF, 2);
}

static void ui_divider(uint32_t y, uint16_t color)
{
    lcd_fill(UI_LEFT, y, UI_WIDTH, 1, color);
}

static void ui_tape_outline(uint32_t y, uint32_t height, uint16_t color)
{
    lcd_fill(8, y, 224, 2, color);
    lcd_fill(8, y + height - 2u, 224, 2, color);
    lcd_fill(8, y + 2u, 2, height - 4u, color);
    lcd_fill(230, y + 2u, 2, height - 4u, color);
}

static void ui_setting(uint32_t y, const char *label, const char *value)
{
    ui_text(UI_LEFT, y, 100, &UI_BODY, label, C_KEY, 0);
    ui_text(115, y - 1u, 110, &UI_LARGE, value, C_ACC, 2);
    ui_divider(y + 22u, *label ? C_RULE : C_BG);
}

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
#define MAX_TUNING_CENTS 50
static struct {
    uint8_t oct, ch;
    uint8_t internal_clock;
    int8_t tuning;
} set;

static void load_global_settings(void)
{
    if (settings_load(&set, sizeof set)) {
        /* Older firmware wrote only octave and MIDI channel. */
        uint8_t legacy[2];
        if (settings_load(legacy, sizeof legacy) == 0) {
            set.oct = legacy[0];
            set.ch = legacy[1];
        }
    }
    if ((int8_t)set.oct < -2 || (int8_t)set.oct > 2) set.oct = 0;
    if (set.ch > 16) set.ch = 0;
    if (set.internal_clock > 1) set.internal_clock = 0;
    if (set.tuning < -MAX_TUNING_CENTS || set.tuning > MAX_TUNING_CENTS) set.tuning = 0;
    pluck_set_tuning(set.tuning);
}
static uint32_t idle_ok, clk_n, clk_t0, clk_bpm, last_clock_ms;
static arp_t arp = {
    .rate = ARP_EIGHTH, .gate = 65, .bpm = 120, .spread_ms = 20,
    .active_note = ARP_NO_NOTE, .last_note = ARP_NO_NOTE
};
/* One held velocity per input and channel preserves notes when switching modes. */
static uint8_t input_velocity[OWNER_TAPE][128];
static uint8_t arp_midi_channel[128];

static uint8_t midi_channel(void)
{
    return set.ch ? set.ch - 1u : 0;
}

static void arp_output(uint8_t note, uint8_t velocity)
{
    if (velocity) {
        play(OWNER_ARP, note, velocity);
        arp_midi_channel[note] = midi_channel();
        midi_send(0x90u | arp_midi_channel[note], note, velocity);
    } else {
        stop(OWNER_ARP, note);
        midi_send(0x80u | arp_midi_channel[note], note, 0);
    }
}

static void direct_input(uint8_t owner, uint8_t note, uint8_t velocity)
{
    if (velocity) play(owner, note, velocity);
    else stop(owner, note);
    if (owner < OWNER_USB)
        midi_send((velocity ? 0x90u : 0x80u) | midi_channel(), note, velocity);
}

static void input_on(uint8_t owner, uint8_t note, uint8_t velocity)
{
    if (note > 127u || velocity == 0 || velocity > 127u) return;
    uint8_t held = input_velocity[owner][note];
    input_velocity[owner][note] = velocity;
    arp_hold(&arp, note, velocity, held != 0);
    if (arp.mode == ARP_OFF) direct_input(owner, note, velocity);
}

static void input_off(uint8_t owner, uint8_t note)
{
    if (note > 127u || !input_velocity[owner][note]) return;
    input_velocity[owner][note] = 0;
    arp_release(&arp, note, arp_output);
    if (arp.refs[note]) {
        uint8_t velocity = 0;
        for (uint32_t i = 0; i < OWNER_TAPE; i++)
            if (input_velocity[i][note] > velocity) velocity = input_velocity[i][note];
        arp.velocity[note] = velocity;
    }
    if (arp.mode == ARP_OFF) direct_input(owner, note, 0);
}

static void release_live_notes(void)
{
    for (uint32_t owner = 0; owner < OWNER_TAPE; owner++)
        for (uint32_t note = 0; note < 128; note++)
            input_off((uint8_t)owner, (uint8_t)note);
    arp_set_mode(&arp, arp.mode, arp_output);
}

static int set_midi_channel(int32_t channel)
{
    uint8_t next = (uint8_t)(channel < 0 ? 0 : channel > 16 ? 16 : channel);
    if (next == set.ch) return 0;
    /* Use the old channel for any note-offs owed to an external instrument. */
    release_live_notes();
    set.ch = next;
    return 1;
}

static void change_arp_mode(uint8_t mode)
{
    if (mode == arp.mode) return;
    uint8_t direct_before = arp.mode == ARP_OFF;
    arp_set_mode(&arp, mode, arp_output);
    if (!direct_before && mode != ARP_OFF) return;
    for (uint32_t owner = 0; owner < OWNER_TAPE; owner++) {
        for (uint32_t note = 0; note < 128; note++) {
            uint8_t velocity = input_velocity[owner][note];
            if (velocity) direct_input((uint8_t)owner, (uint8_t)note, direct_before ? 0 : velocity);
        }
    }
}

static uint32_t external_bpm(void)
{
    return !set.internal_clock && clk_bpm >= 30 && clk_bpm <= 300 &&
           fm1_ms - last_clock_ms < 500u ? clk_bpm : 0;
}

static void service_arp(void)
{
    uint32_t external = external_bpm();
    arp_tick(&arp, fm1_ms, external ? 60000u / external : 0, clk_n, arp_output);
}

static void midi_in(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {   /* 1 USB, 2 the TRS jack */
        uint32_t ty = st & 0xF0u;
        if (st == 0xF8u) {
            last_clock_ms = fm1_ms;                      /* clock: 24 a quarter note */
            if (clk_n % 24u == 0u) {
                if (clk_n) {
                    uint32_t ms = fm1_ms - clk_t0;   /* a quarter's length */
                    clk_bpm = ms ? (60000u + ms / 2u) / ms : 0;
                }
                clk_t0 = fm1_ms;
            }
            clk_n++;
            continue;
        }
        if (st == 0xFAu) { clk_n = 0; continue; }   /* START: the next clock is beat 1 */
        if (st == 0xFBu || st == 0xFCu) continue;                /* CONTINUE / STOP */
        uint8_t owner = (uint8_t)((src == 2 ? OWNER_TRS : OWNER_USB) + (st & 15u));
        if (ty == 0x90u && d2) {
            if (set.ch && (st & 15u) != set.ch - 1u) continue;
            input_on(owner, d1, d2);
        } else if (ty == 0x80u || ty == 0x90u) {
            input_off(owner, d1);
        } else if (ty == 0xb0u && (d1 == 120u || d1 == 123u)) {
            for (uint32_t note = 0; note < 128; note++) input_off(owner, (uint8_t)note);
        }
    }
}
static void app_idle(void)
{
    if (!idle_ok) return;
    advance_tapes();
    midi_in();
    service_arp();
}

static void draw_macros(const pluck_part_t *sound)
{
    for (uint32_t i = 0; i < PLUCK_MACROS; i++) {
        uint32_t y = 53u + i * 40u;
        ui_text(UI_LEFT, y, UI_WIDTH, &UI_BODY, pluck_names[i], C_KEY, 0);
        cv_begin(UI_WIDTH, 8, C_BG);
        cv_rect(0, 2, UI_WIDTH, 1, C_RULE);
        cv_rect(0, 1, sound->macro[i] * UI_WIDTH / 127u, 3, C_ACC);
        cv_rect(0, 7, UI_WIDTH, 1, C_RULE);
        cv_blit(UI_LEFT, y + 18u);
    }
    ui_setting(137, "RESONATOR", resonator_names[sound->resonator]);
    ui_text(UI_LEFT, 176, 78, &UI_BODY, "PING", C_KEY, 0);
    ui_text(93, 175, 132, &UI_LARGE, ping_names[sound->ping], C_ACC, 2);
    ui_divider(198, C_RULE);
}

static void format_percent(char value[5], uint8_t percent);

static void draw_home_mixer(uint8_t page)
{
    for (uint32_t i = 0; i < TAPE_COUNT; i++) {
        const pluck_part_t *part = &pluck_parts[i];
        uint32_t y = 56u + i * 40u;
        uint8_t selected = i == armed_tape;
        uint16_t color = selected ? C_ACC : C_KEY;
        char tape[] = "TAPE 1";
        tape[5] = (char)('1' + i);
        ui_text(UI_LEFT, y, 70, &UI_BODY, tape, color, 0);
        ui_tape_outline(y - 6u, 31u, selected ? C_ACC : C_BG);
        if (!selected) ui_divider(y + 23u, C_RULE);
        if (page == PAGE_MUTE) {
            ui_text(141, y - 1u, 84, &UI_LARGE, part->muted ? "MUTED" : "ON",
                    part->muted ? C_ACC : color, 2);
            continue;
        }
        if (page == PAGE_HOME) {
            char value[5];
            format_percent(value, (uint8_t)((part->level * 100u + 63u) / PLUCK_MAX_LEVEL));
            ui_text(174, y - 1u, 51, &UI_LARGE, value, color, 2);
            cv_begin(79, 9, C_BG);
            cv_rect(0, 4, 79, 1, C_RULE);
            uint32_t width = (part->level * 79u + PLUCK_MAX_LEVEL - 1u) / PLUCK_MAX_LEVEL;
            if (width) cv_rect(0, 3, width, 3, color);
            cv_blit(91, y + 5);
            continue;
        }
        int32_t pan = part->pan;
        uint32_t amount = (uint32_t)(pan < 0 ? -pan : pan);
        char value[5] = "C";
        if (amount) {
            uint32_t pos = 0;
            value[pos++] = pan < 0 ? 'L' : 'R';
            if (amount >= 100u) value[pos++] = '1';
            if (amount >= 10u) value[pos++] = (char)('0' + amount / 10u % 10u);
            value[pos++] = (char)('0' + amount % 10u);
            value[pos] = 0;
        }
        ui_text(174, y - 1u, 51, &UI_LARGE, value, color, 2);
        cv_begin(79, 11, C_BG);
        cv_rect(0, 5, 79, 1, C_RULE);
        cv_rect(39, 1, 1, 9, C_RULE);
        cv_rect(38 + pan * 38 / PLUCK_PAN_MAX, 1, 2, 9, color);
        cv_blit(91, y + 4);
    }
}

static void edit_sound(pluck_part_t *sound, const int32_t steps[TAPE_COUNT])
{
    for (uint32_t i = 0; i < PLUCK_MACROS; i++) {
        int32_t value = sound->macro[i] + steps[i] * 4;
        sound->macro[i] = (uint8_t)(value < 0 ? 0 : value > 127 ? 127 : value);
    }
    int32_t resonator = sound->resonator + steps[2];
    sound->resonator = (uint8_t)(resonator < RESONATOR_STRING ? RESONATOR_STRING :
                                 resonator > RESONATOR_BLOW ? RESONATOR_BLOW : resonator);
    int32_t ping = sound->ping + steps[3];
    sound->ping = (uint8_t)(ping < 0 ? 0 : ping >= PING_COUNT ? PING_COUNT - 1 : ping);
}

static void edit_tape_lengths(const int32_t steps[TAPE_COUNT], int32_t mode_step)
{
    for (uint32_t i = 0; i < TAPE_COUNT; i++) {
        uint8_t mode = tapes[i].mode;
        if (i == armed_tape && mode_step) mode = mode_step > 0 ? TAPE_STRETCH : TAPE_KEEP;
        if (!steps[i] && mode == tapes[i].mode) continue;
        int32_t length = tapes[i].length_ms + steps[i] * 1000;
        length = length < TAPE_MIN_MS ? TAPE_MIN_MS : length > TAPE_MAX_MS ? TAPE_MAX_MS : length;
        tape_set_timing(&tapes[i], (uint16_t)length, mode, tape_event);
    }
}

#define TIMELINE_W UI_WIDTH

static uint32_t tape_screen_x(const tape_t *tape, uint16_t ms)
{
    return (uint32_t)ms * (TIMELINE_W - 2u) / tape->length_ms;
}

static void draw_tape_label(uint32_t track, uint32_t y, uint16_t color)
{
    const tape_t *tape = &tapes[track];
    uint32_t seconds = tape->length_ms / 1000u;
    uint32_t free = TAPE_MAX_NOTES - tape->count;
    char label[] = "1 08s  64";
    label[0] = (char)('1' + track);
    label[2] = (char)('0' + seconds / 10u);
    label[3] = (char)('0' + seconds % 10u);
    label[7] = (char)('0' + free / 10u);
    label[8] = (char)('0' + free % 10u);
    ui_text(UI_LEFT, y, 100, &UI_BODY, label, color, 0);
    ui_text(115, y + 2u, 62, &UI_SMALL, tape->mode == TAPE_STRETCH ? "STRETCH" : "KEEP", color, 1);
    const char *state = tape->recording ? "REC" : pluck_parts[track].muted ? "MUTE" :
                        tape->playing ? "PLAY" : "STOP";
    uint16_t state_color = tape->recording ? RGB(255, 80, 80) : C_OFF;
    ui_text(190, y + 2u, 35, &UI_SMALL, state, state_color, 2);
}

static void draw_tape_row(uint32_t track, uint32_t *drawn)
{
    const tape_t *tape = &tapes[track];
    uint32_t head = tape_screen_x(tape, tape->position_ms);
    uint32_t selected = track == armed_tape;
    uint32_t state = head | (uint32_t)tape->count << 8 | (uint32_t)tape->playing << 15 |
                     (uint32_t)tape->recording << 16 | selected << 17 |
                     (uint32_t)(tape->length_ms / 1000u) << 18 | (uint32_t)tape->mode << 23 |
                     (uint32_t)pluck_parts[track].muted << 24;
    if (state == *drawn) return;
    uint32_t y = 52u + track * 40u;
    uint16_t color = selected ? C_ACC : C_KEY;
    if (*drawn == ~0u || ((*drawn ^ state) & (1u << 17)))
        ui_tape_outline(y - 6u, 36u, selected ? C_ACC : C_BG);
    if ((*drawn >> 8) != (state >> 8)) draw_tape_label(track, y, color);
    *drawn = state;

    cv_begin(TIMELINE_W, 10, C_BG);
    cv_rect(0, 4, TIMELINE_W, 1, C_RULE);
    for (uint32_t i = 0; i < tape->count; i++) {
        uint16_t start = tape->notes[i].play_start_ms;
        if (start < tape->length_ms)
            cv_rect(tape_screen_x(tape, start), 2, 2, 6, C_OFF);
    }
    cv_rect(head, 0, 2, 10, C_ACC);
    cv_blit(UI_LEFT, y + 18u);
}

static void draw_leak_row(uint32_t track, uint32_t *drawn)
{
    uint8_t passes = tapes[track].leak_passes;
    uint8_t selected = track == armed_tape;
    uint32_t state = passes | (uint32_t)selected << 8;
    if (state == *drawn) return;
    *drawn = state;

    char label[] = "TAPE 1";
    label[5] = (char)('1' + track);
    char value[] = "10 PASSES";
    if (passes < 10) {
        value[0] = ' ';
        value[1] = (char)('0' + passes);
    }
    uint32_t y = 56 + track * 40u;
    ui_text(UI_LEFT, y, 85, &UI_BODY, label, selected ? C_ACC : C_KEY, 0);
    ui_text(100, y - 1u, 125, &UI_LARGE, passes ? value : "OFF", C_ACC, 2);
    ui_tape_outline(y - 6u, 31u, selected ? C_ACC : C_BG);
    if (!selected) ui_divider(y + 23u, C_RULE);
}

static uint8_t limit_setting(int32_t value, int32_t low, int32_t high)
{
    return (uint8_t)(value < low ? low : value > high ? high : value);
}

static uint8_t step_arp_setting(uint8_t value, int32_t step, const uint8_t *order, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (order[i] == value) return order[limit_setting((int32_t)i + step, 0, count - 1)];
    return value;
}

static void edit_arp(const int32_t steps[TAPE_COUNT])
{
    static const uint8_t mode_order[] = {
        ARP_OFF, ARP_UP, ARP_DOWN, ARP_UP_DOWN, ARP_RANDOM, ARP_STRUM_UP, ARP_STRUM_DOWN
    };
    static const uint8_t rate_order[] = {
        ARP_HALF, ARP_QUARTER, ARP_EIGHTH, ARP_EIGHTH_TRIPLET,
        ARP_SIXTEENTH, ARP_SIXTEENTH_TRIPLET, ARP_THIRTYSECOND
    };
    if (steps[0]) change_arp_mode(step_arp_setting(arp.mode, steps[0], mode_order, sizeof mode_order));
    if (arp_is_strum(&arp)) {
        arp.spread_ms = limit_setting(arp.spread_ms + steps[1] * 5, 5, 80);
    } else {
        if (steps[1]) {
            uint8_t rate = step_arp_setting(arp.rate, steps[1], rate_order, sizeof rate_order);
            if (rate != arp.rate) arp.running = 0;
            arp.rate = rate;
        }
        arp.gate = limit_setting(arp.gate + steps[2] * 5, 10, 100);
        arp.bpm = limit_setting(arp.bpm + steps[3], 40, 240);
    }
}

static void draw_setting_row(uint32_t row, const char *label, const char *value)
{
    ui_setting(56u + row * 40u, label, value);
}

static void format_percent(char value[5], uint8_t percent)
{
    value[0] = percent == 100 ? '1' : ' ';
    value[1] = percent < 10 ? ' ' : (char)('0' + percent / 10 % 10);
    value[2] = (char)('0' + percent % 10);
    value[3] = '%';
    value[4] = 0;
}

static void draw_arp(uint32_t external)
{
    draw_setting_row(0, "MODE", arp_mode_names[arp.mode]);
    if (arp_is_strum(&arp)) {
        char spread[] = "20 MS";
        spread[0] = arp.spread_ms < 10 ? ' ' : (char)('0' + arp.spread_ms / 10);
        spread[1] = (char)('0' + arp.spread_ms % 10);
        draw_setting_row(1, "SPREAD", spread);
        draw_setting_row(2, "", "");
        draw_setting_row(3, "", "");
        return;
    }
    char gate[5];
    format_percent(gate, arp.gate);
    uint32_t bpm = external ? external : arp.bpm;
    char tempo[] = "120 BPM";
    tempo[0] = bpm < 100 ? ' ' : (char)('0' + bpm / 100);
    tempo[1] = (char)('0' + bpm / 10 % 10);
    tempo[2] = (char)('0' + bpm % 10);
    draw_setting_row(1, "RATE", arp_rate_names[arp.rate]);
    draw_setting_row(2, "GATE", gate);
    draw_setting_row(3, external ? "MIDI TEMPO" : "TEMPO", tempo);
}

static const char *const delay_division_names[] = {"1/16", "1/8", "1/8 D", "1/4", "1/4 D"};
static const uint8_t delay_division_clocks[] = {6, 12, 18, 24, 36};
static struct { uint8_t on, division, feedback, mix; } fx = {0, 3, 35, 30};

static void edit_fx(const int32_t steps[TAPE_COUNT])
{
    if (steps[0]) fx.on = steps[0] > 0;
    fx.division = limit_setting(fx.division + steps[1], 0,
                                sizeof delay_division_clocks / sizeof delay_division_clocks[0] - 1);
    fx.feedback = limit_setting(fx.feedback + steps[2] * 5, 0, 85);
    fx.mix = limit_setting(fx.mix + steps[3] * 5, 0, 100);
}

static uint32_t fx_bpm(void)
{
    uint32_t bpm = external_bpm();
    return bpm ? bpm : arp.bpm;
}

static void update_fx(void)
{
    static uint32_t previous = ~0u;
    uint32_t bpm = fx_bpm();
    uint32_t state = bpm | (uint32_t)fx.division << 9 | (uint32_t)fx.feedback << 12 |
                     (uint32_t)fx.mix << 19 | (uint32_t)fx.on << 26;
    if (state == previous) return;
    previous = state;
    delay_configure(fx.on, delay_tempo_samples(bpm, delay_division_clocks[fx.division]),
                    fx.feedback, fx.mix);
}

static void draw_fx(void)
{
    char feedback[5], mix[5];
    format_percent(feedback, fx.feedback);
    format_percent(mix, fx.mix);
    draw_setting_row(0, "EFFECT", fx.on ? "DELAY" : "OFF");
    draw_setting_row(1, "TIME", delay_division_names[fx.division]);
    draw_setting_row(2, "FEEDBACK", feedback);
    draw_setting_row(3, "MIX", mix);
}

static void draw_delay_footer(void)
{
    uint32_t bpm = fx_bpm();
    char tempo[] = "ARP   120 BPM";
    if (external_bpm()) {
        tempo[0] = 'M'; tempo[1] = 'I'; tempo[2] = 'D'; tempo[3] = 'I';
    }
    tempo[6] = bpm < 100 ? ' ' : (char)('0' + bpm / 100);
    tempo[7] = (char)('0' + bpm / 10 % 10);
    tempo[8] = (char)('0' + bpm % 10);
    ui_footer_pair("FX: HALL", tempo);
}

static struct { uint8_t on, decay, damping, mix; } hall_settings = {0, 65, 35, 25};

static void edit_hall(const int32_t steps[TAPE_COUNT])
{
    if (steps[0]) hall_settings.on = steps[0] > 0;
    hall_settings.decay = limit_setting(hall_settings.decay + steps[1] * 5, 0, 100);
    hall_settings.damping = limit_setting(hall_settings.damping + steps[2] * 5, 0, 100);
    hall_settings.mix = limit_setting(hall_settings.mix + steps[3] * 5, 0, 100);
    hall_configure(hall_settings.on, hall_settings.decay, hall_settings.damping, hall_settings.mix);
}

static void draw_hall(void)
{
    char decay[5], damping[5], mix[5];
    format_percent(decay, hall_settings.decay);
    format_percent(damping, hall_settings.damping);
    format_percent(mix, hall_settings.mix);
    draw_setting_row(0, "EFFECT", hall_settings.on ? "HALL" : "OFF");
    draw_setting_row(1, "DECAY", decay);
    draw_setting_row(2, "DAMPING", damping);
    draw_setting_row(3, "MIX", mix);
}

static int page_is_home(uint8_t page)
{
    return page == PAGE_HOME || page == PAGE_PAN || page == PAGE_MUTE;
}

static int page_is_fx(uint8_t page)
{
    return page == PAGE_FX || page == PAGE_HALL;
}

#define ENV_MAX_MS 1500u

static void edit_env(pluck_adsr_t *adsr, const int32_t steps[TAPE_COUNT])
{
    int32_t attack = adsr->attack_ms + steps[0] * 10;
    int32_t decay = adsr->decay_ms + steps[1] * 10;
    int32_t release = adsr->release_ms + steps[3] * 10;
    adsr->attack_ms = (uint16_t)(attack < 0 ? 0 : attack > (int32_t)ENV_MAX_MS ? (int32_t)ENV_MAX_MS : attack);
    adsr->decay_ms = (uint16_t)(decay < 0 ? 0 : decay > (int32_t)ENV_MAX_MS ? (int32_t)ENV_MAX_MS : decay);
    adsr->sustain = limit_setting(adsr->sustain + steps[2] * 5, 0, 100);
    adsr->release_ms = (uint16_t)(release < 0 ? 0 : release > (int32_t)ENV_MAX_MS ? (int32_t)ENV_MAX_MS : release);
}

static void draw_knob_value(uint32_t knob, uint32_t top, const char *label, const char *value)
{
    uint32_t x = knob % 2u ? 124u : UI_LEFT;
    uint32_t y = top + knob / 2u * 34u;
    ui_text(x, y, 101, &UI_SMALL, label, C_OFF, 0);
    while (*value == ' ') value++;
    ui_text(x, y + 13u, 101, &UI_BODY, value, C_ACC, 0);
}

static int edit_global(const int32_t steps[TAPE_COUNT])
{
    uint8_t clock = steps[0] ? steps[0] > 0 : set.internal_clock;
    int32_t tuning = set.tuning + steps[2];
    if (tuning < -MAX_TUNING_CENTS) tuning = -MAX_TUNING_CENTS;
    if (tuning > MAX_TUNING_CENTS) tuning = MAX_TUNING_CENTS;
    int changed = set_midi_channel(set.ch + steps[1]);
    if (clock != set.internal_clock) {
        set.internal_clock = clock;
        arp.running = 0;
        changed = 1;
    }
    if (tuning != set.tuning) {
        set.tuning = (int8_t)tuning;
        pluck_set_tuning(tuning);
        changed = 1;
    }
    if (steps[3] > 0) release_live_notes();
    return changed;
}

static void draw_global(void)
{
    char channel[] = "CH 00";
    channel[3] = (char)('0' + set.ch / 10);
    channel[4] = (char)('0' + set.ch % 10);
    int32_t magnitude = set.tuning < 0 ? -set.tuning : set.tuning;
    char tuning[] = "+00 CENTS";
    tuning[1] = (char)('0' + magnitude / 10);
    tuning[2] = (char)('0' + magnitude % 10);
    char *signed_tuning = tuning + (magnitude < 10);
    *signed_tuning = set.tuning < 0 ? '-' : '+';
    draw_knob_value(0, 92, "1 CLOCK", set.internal_clock ? "INTERNAL" : "AUTO");
    draw_knob_value(1, 92, "2 MIDI IN", set.ch ? channel : "OMNI");
    draw_knob_value(2, 92, "3 TUNING", magnitude ? signed_tuning : "0 CENTS");
    draw_knob_value(3, 92, "4 PANIC", "TURN RIGHT");
}

static void draw_env_value(uint32_t knob, const char *label, uint32_t amount, int percent)
{
    char value[8];
    uint32_t digits = amount >= 1000 ? 4 : amount >= 100 ? 3 : amount >= 10 ? 2 : 1;
    for (uint32_t i = digits; i > 0; i--) {
        value[i - 1] = (char)('0' + amount % 10);
        amount /= 10;
    }
    value[digits] = percent ? '%' : 'M';
    value[digits + 1] = percent ? 0 : 'S';
    value[digits + 2] = 0;
    draw_knob_value(knob, 130, label, value);
}

static void draw_env(const pluck_adsr_t *adsr)
{
    char title[] = "TAPE 1  ENV";
    title[5] = (char)('1' + armed_tape);
    ui_title(title);

    /* Sustain occupies a fixed display span because its duration is set by key-up. */
    int32_t times[] = {adsr->attack_ms, adsr->decay_ms, 700, adsr->release_ms};
    int32_t total = times[0] + times[1] + times[2] + times[3];
    int32_t x[5] = {0};
    int32_t elapsed = 0;
    for (uint32_t i = 0; i < 4; i++) {
        elapsed += times[i];
        x[i + 1] = elapsed * (int32_t)(UI_WIDTH - 1u) / total;
    }
    int32_t sustain = 67 - (int32_t)adsr->sustain * 61 / 100;
    int32_t y[] = {67, 6, sustain, sustain, 67};
    cv_begin(UI_WIDTH, 70, C_BG);
    for (uint32_t i = 0; i < 4; i++) {
        int32_t width = x[i + 1] - x[i];
        for (int32_t col = 0; col < width; col++) {
            int32_t top = y[i] + (y[i + 1] - y[i]) * col / width;
            cv_rect(x[i] + col, top, 1, 68 - top, C_GRAPH);
        }
    }
    for (uint32_t i = 1; i < 4; i++)
        for (int32_t row = y[i] + 3; row < 67; row += 4)
            cv_pset(x[i], row, C_RULE);
    for (uint32_t i = 0; i < 4; i++)
        cv_line(x[i], y[i], x[i + 1], y[i + 1], C_ACC);
    cv_blit(UI_LEFT, 51);

    draw_env_value(0, "1 ATK", adsr->attack_ms, 0);
    draw_env_value(1, "2 DEC", adsr->decay_ms, 0);
    draw_env_value(2, "3 SUS", adsr->sustain, 1);
    draw_env_value(3, "4 REL", adsr->release_ms, 0);
}

static void edit_lfo(pluck_lfo_t *lfo, const int32_t steps[TAPE_COUNT])
{
    lfo->target = limit_setting(lfo->target + steps[0], 0, LFO_TARGETS - 1);
    lfo->wave = limit_setting(lfo->wave + steps[1], 0, LFO_WAVES - 1);
    pluck_lfo_rate(lfo, limit_setting(lfo->rate_tenths + steps[2], 1, 120));
    lfo->depth = limit_setting(lfo->depth + steps[3] * 5, 0, 100);
}

static void draw_lfo(const pluck_lfo_t *lfo)
{
    static const char *const targets[] = {"PITCH", "VOLUME", "BRIGHTNESS", "POSITION"};
    static const char *const waves[] = {"SINE", "TRIANGLE", "SQUARE", "SAW", "S&H"};
    static const int16_t held[] = {18000, -11000, 29000, 7000, -28000, 13000};
    char title[] = "TAPE 1  LFO";
    title[5] = (char)('1' + armed_tape);
    ui_title(title);

    cv_begin(UI_WIDTH, 70, C_BG);
    cv_rect(0, 35, UI_WIDTH, 1, C_RULE);
    int32_t previous_y = 35;
    uint8_t wave = lfo->wave;
    for (uint32_t x = 0; x < UI_WIDTH; x++) {
        int32_t sample = pluck_lfo_wave(wave, x * 131072u / UI_WIDTH % 65536u,
                                        held[x * 6u / UI_WIDTH]);
        int32_t y = 35 - sample * lfo->depth * 30 / (32768 * 100);
        if (x) cv_line(x - 1, previous_y, x, y, C_ACC);
        previous_y = y;
    }
    cv_blit(UI_LEFT, 51);

    uint8_t rate = lfo->rate_tenths;
    char hz[] = " 0.0 HZ";
    hz[0] = rate >= 100 ? '1' : ' ';
    hz[1] = (char)('0' + rate / 10 % 10);
    hz[3] = (char)('0' + rate % 10);
    char depth[5];
    format_percent(depth, lfo->depth);
    draw_knob_value(0, 130, "1 TARGET", targets[lfo->target]);
    draw_knob_value(1, 130, "2 WAVE", waves[wave]);
    draw_knob_value(2, 130, "3 RATE", hz);
    draw_knob_value(3, 130, "4 DEPTH", depth);
}

static void draw_scale(void)
{
    static const char *const roots[] = {"C", "C#", "D", "D#", "E", "F",
                                        "F#", "G", "G#", "A", "A#", "B"};
    static const uint8_t white[] = {0, 2, 4, 5, 7, 9, 11};
    static const uint8_t black[] = {1, 3, 6, 8, 10};
    static const uint8_t black_x[] = {30, 60, 120, 150, 180};
    uint16_t mask = scales[global_scale].steps;
    uint8_t root = effective_tape_root();
    char root_label[] = "C  > C ";
    root_label[0] = roots[scale_root][0];
    root_label[1] = roots[scale_root][1] ? '#' : ' ';
    root_label[5] = roots[root][0];
    root_label[6] = roots[root][1] ? '#' : ' ';

    ui_text(UI_LEFT, 54, 62, &UI_BODY, "ROOT", C_KEY, 0);
    ui_text(77, 53, 148, &UI_LARGE, transpose_root == NO_TRANSPOSE_ROOT ? roots[scale_root] : root_label, C_ACC, 2);
    ui_divider(76, C_RULE);
    ui_text(UI_LEFT, 86, 60, &UI_BODY, "SCALE", C_KEY, 0);
    ui_text(75, 86, 150, &UI_BODY, scales[global_scale].name, C_ACC, 2);
    ui_divider(108, C_RULE);

    cv_begin(UI_WIDTH, 69, C_BG);
    for (uint32_t i = 0; i < 7; i++) {
        uint16_t color = scale_contains(mask, root, white[i]) ? C_ACC : C_GRAPH;
        cv_rect(i * 30u, 0, 28, 69, C_RULE);
        cv_rect(i * 30u + 1, 1, 26, 67, color);
    }
    for (uint32_t i = 0; i < 5; i++) {
        int32_t x = black_x[i] - 10;
        cv_rect(x, 0, 20, 42, C_RULE);
        cv_rect(x + 1, 0, 18, 41, C_BG);
        uint16_t color = scale_contains(mask, root, black[i]) ? C_ACC : C_BG;
        cv_rect(x + 2, 1, 16, 39, color);
    }
    cv_blit(UI_LEFT, 129);
}

static uint32_t random_setting(uint32_t *state, uint32_t choices)
{
    *state = *state * 1664525u + 1013904223u;
    return (*state >> 16) % choices;
}

static void randomize_sound(pluck_part_t *sound, uint32_t *state)
{
    sound->macro[PLUCK_BRIGHTNESS] = (uint8_t)random_setting(state, 128);
    sound->macro[PLUCK_POSITION] = (uint8_t)random_setting(state, 128);
    sound->resonator = (uint8_t)random_setting(state, RESONATOR_BLOW + 1u);
    sound->ping = (uint8_t)random_setting(state, PING_COUNT);
    sound->adsr.attack_ms = (uint16_t)(random_setting(state, ENV_MAX_MS / 10u + 1u) * 10u);
    sound->adsr.decay_ms = (uint16_t)(random_setting(state, ENV_MAX_MS / 10u + 1u) * 10u);
    sound->adsr.sustain = (uint8_t)(random_setting(state, 21) * 5u);
    sound->adsr.release_ms = (uint16_t)(random_setting(state, ENV_MAX_MS / 10u + 1u) * 10u);
    sound->lfo.target = (uint8_t)random_setting(state, LFO_TARGETS);
    sound->lfo.wave = (uint8_t)random_setting(state, LFO_WAVES);
    pluck_lfo_rate(&sound->lfo, (uint8_t)(1u + random_setting(state, 120)));
    sound->lfo.depth = (uint8_t)(random_setting(state, 21) * 5u);
}

static void transpose_key_up(uint8_t *order, uint8_t *count, uint8_t key)
{
    for (uint32_t i = 0; i < *count; i++) {
        if (order[i] != key) continue;
        --*count;
        for (; i < *count; i++) order[i] = order[i + 1];
        return;
    }
}

#include "snapshot.h"

static uint32_t boot_sound_seed(void)
{
    uint32_t seed = fm1_random_word();
    if (seed != 0 && seed != 0x0bf34dadu) return seed;
    uint32_t second = fm1_random_word();

    /* fm1-emu starts JL_RAND at a fixed seed. Older builds return zero. */
    if ((seed || second) && (seed != 0x0bf34dadu || second != 0x026e6076u))
        return seed ? seed : second;

    /* Keep the emulator's seed in its own blob, separate from saved sounds. */
    const uint32_t boot_seed_blob = SNAPSHOT_FIRST_BLOB - 1u;
    uint32_t saved = 0;
    blob_load(boot_seed_blob, &saved, sizeof saved);
    saved += 0x9e3779b9u;
    blob_save(boot_seed_blob, &saved, sizeof saved);
    return saved ^ seed;
}

static void draw_page_footer(uint8_t page, uint8_t erasing)
{
    if (snapshot_message) {
        ui_text(UI_LEFT, UI_FOOTER_Y, UI_WIDTH, &UI_SMALL, snapshot_message, C_ACC, 1);
        return;
    }

    switch (page) {
    case PAGE_HOME: ui_footer("HOME: PAN"); break;
    case PAGE_PAN:  ui_footer("HOME: MUTE"); break;
    case PAGE_MUTE: ui_footer("LEFT: MUTE  RIGHT: ON"); break;
    case PAGE_EDIT: ui_footer("LONG PRESS EDIT: RANDOMISE"); break;
    case PAGE_ENV:
    case PAGE_LFO:  ui_footer("SELECT: TAPE"); break;
    case PAGE_LEAK: ui_footer("SEQ: BACK TO TAPES"); break;
    case PAGE_TAPE:
        if (erasing)
            ui_text(UI_LEFT, UI_FOOTER_Y, UI_WIDTH, &UI_SMALL, "ERASING", RGB(255, 80, 80), 1);
        else
            ui_footer_pair("SEQ: LEAK", "EDIT: ERASE");
        break;
    case PAGE_FX:   draw_delay_footer(); break;
    case PAGE_HALL: ui_footer_pair("FX: DELAY", "12 BIT / 20 KHZ"); break;
    case PAGE_SCALE:
        ui_footer(tapes[armed_tape].playing && !can_transpose_tapes() ?
                  "RECORDING" : "KEYS: TRANSPOSE");
        break;
    default:        ui_footer(""); break;
    }
}

static void app_main(void)
{
    uint32_t prev = 0, k, t_change = 0, dirty = 0;
    uint32_t tape_drawn[TAPE_COUNT];
#if !BOARD_SMK37
    uint8_t battery_drawn = UINT8_MAX;
#endif
    uint8_t page = PAGE_HOME, edit_hold_pending = 0, erase_drawn = 0;
    uint8_t home_page = PAGE_HOME, seq_page = PAGE_TAPE, fx_page = PAGE_FX;
    uint32_t edit_hold_started = 0;
    uint32_t external_drawn = ~0u, fx_tempo_drawn = ~0u;
    uint32_t transpose_keys = 0;
    uint8_t transpose_order[27], transpose_count = 0;
    uint8_t held[27] = {0};                     /* the note each key plays */
#if BOARD_SMK37
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* white key -> key */
    uint8_t pad_note[16] = {0}, ext_note[FM1_NKEY] = {0};
    smk_pw_t wheel = {0};
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here, lit while they sound */
#endif
    load_global_settings();

    uint32_t sound_random = boot_sound_seed();
    for (uint32_t i = 0; i < TAPE_COUNT; i++) {
        pluck_set_level(&pluck_parts[i], PLUCK_MAX_LEVEL);
        randomize_sound(&pluck_parts[i], &sound_random);
        pluck_lfo_state[i].random = 0x9e3779b9u * (i + 1u);
    }

    snapshot_find();
    draw_boot_logo();
    uint32_t boot_started = fm1_ms;
    uint8_t showing_boot = 1;
    tape_ms = fm1_ms;
    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();
#if !BOARD_SMK37
        uint8_t battery_level = read_battery_level(fm1_ms);
#endif
        advance_tapes();
        b = fm1_input_edges(&rel);
        uint8_t previous_armed = armed_tape;
        tape_select(fm1_enc_take(ENC_SELECT));
        tape_transport(b);

        int transpose_available = can_transpose_tapes();
        if (!transpose_available) transpose_count = 0;
        uint8_t transpose_input = (page == PAGE_SCALE || (b & (1u << BTN_SEL))) &&
                                  transpose_available;
        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {                 /* (a key stops the note it started: the octave may change meanwhile) */
#if BOARD_SMK37
            uint32_t vel = fm1_in.key_vel[SMK_KEY_F3 + k];   /* the SMK's keys feel the velocity */
#else
            uint32_t vel = 100;
#endif
            uint32_t key_bit = 1u << k;
            if (down & key_bit) {
                held[k] = (uint8_t)(base + k);
                if (transpose_input) {
                    transpose_keys |= key_bit;
                    transpose_order[transpose_count++] = (uint8_t)k;
                } else {
                    input_on(OWNER_KEYS + k + KEY_OWNER_OFFSET, held[k], (uint8_t)vel);
                }
            }
            if (up & key_bit) {
                if (transpose_keys & key_bit) {
                    transpose_keys &= ~key_bit;
                    transpose_key_up(transpose_order, &transpose_count, (uint8_t)k);
                } else {
                    input_off(OWNER_KEYS + k + KEY_OWNER_OFFSET, held[k]);
                }
            }
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {  /* the keys left and right of the FM-1's 27: key << 8 | velocity */
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key >= FM1_NKEY) continue;
            if (vel) { ext_note[key] = (uint8_t)(base + key - SMK_KEY_F3); input_on(OWNER_KEYS + key, ext_note[key], (uint8_t)vel); }
            else if (ext_note[key]) { input_off(OWNER_KEYS + key, ext_note[key]); ext_note[key] = 0; }
        }
        while ((e = smk_pad_take()) >= 0) {     /* a pad: hardware pad << 8 | velocity (0 = let go) */
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            uint32_t w = pad < 8u ? pad + 8u : pad - 8u;   /* bottom row = white keys 1-8, top row 9-16 */
            if (vel) { pad_note[pad] = (uint8_t)(base + WHITE[w]); input_on(OWNER_PADS + pad, pad_note[pad], (uint8_t)vel); }
            else if (pad_note[pad]) { input_off(OWNER_PADS + pad, pad_note[pad]); pad_note[pad] = 0; }
        }
        {
            int32_t bd = smk_pw_bend(&wheel, fm1_in.wheel[0], fm1_ms);   /* 1/4096 semitone */
            bend_q16 = (uint32_t)(65536 + (bd * 3785 >> 12) + (((bd * bd) >> 12) * 109 >> 12));   /* ~2^(s/12) */
        }
#endif

        uint8_t previous_page = page, macros_dirty = 0;
        if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
        if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        e = fm1_enc_take(ENC_PRESETS);
        if (e && set_midi_channel(set.ch + e)) {
            dirty = 1;
            macros_dirty = page == PAGE_GLO;
            t_change = fm1_ms;
        }

        const uint32_t edit_button = 1u << BTN_EDIT;
        if (b & edit_button) {
            if (page == PAGE_TAPE && (fm1_in.buttons & edit_button) &&
                tapes[armed_tape].playing && !tapes[armed_tape].recording) {
                erase_pending = 1;
                erase_tape = armed_tape;
                erase_started = fm1_ms;
            } else {
                page = PAGE_EDIT;
            }
        }
        if (b & (1u << BTN_ENV)) page = PAGE_ENV;
        if (b & (1u << BTN_LFO)) page = PAGE_LFO;
        if (b & (1u << BTN_SEL)) page = PAGE_SCALE;
        if (b & (1u << BTN_GLO)) page = PAGE_GLO;
        if (b & (1u << BTN_SEQ)) {
            if (page == PAGE_TAPE || page == PAGE_LEAK)
                seq_page = page == PAGE_TAPE ? PAGE_LEAK : PAGE_TAPE;
            page = seq_page;
        }
        if (b & (1u << BTN_HOME)) {
            if (page_is_home(page))
                home_page = page == PAGE_HOME ? PAGE_PAN : page == PAGE_PAN ? PAGE_MUTE : PAGE_HOME;
            page = home_page;
        }
        if (b & (1u << BTN_ARP)) page = PAGE_ARP;
        if (b & (1u << BTN_FX)) {
            if (page_is_fx(page))
                fx_page = page == PAGE_FX ? PAGE_HALL : PAGE_FX;
            page = fx_page;
        }
        if (page == PAGE_TAPE && erase_pending && (rel & edit_button) &&
            armed_tape == erase_tape && tapes[erase_tape].playing && !tapes[erase_tape].recording &&
            fm1_ms - erase_started < ERASE_HOLD_MS)
            page = PAGE_EDIT;
        if (page != PAGE_TAPE || !erase_held()) erase_pending = 0;
        uint8_t erasing = erase_pending && fm1_ms - erase_started >= ERASE_HOLD_MS;
        uint8_t editing = page == PAGE_EDIT;
        int32_t steps[TAPE_COUNT];
        for (k = 0; k < TAPE_COUNT; k++) {
            steps[k] = fm1_enc_take(ENC_KNOB1 + k);
            macros_dirty |= steps[k] != 0;
            if (page == PAGE_HOME && steps[k]) {
                int32_t level = pluck_parts[k].level + steps[k] * 2;
                level = level < 0 ? 0 : level > (int32_t)PLUCK_MAX_LEVEL ? (int32_t)PLUCK_MAX_LEVEL : level;
                pluck_set_level(&pluck_parts[k], (uint8_t)level);
            } else if (page == PAGE_PAN && steps[k]) {
                int32_t pan = pluck_parts[k].pan + steps[k] * 2;
                pluck_parts[k].pan = (int8_t)(pan < -PLUCK_PAN_MAX ? -PLUCK_PAN_MAX :
                                              pan > PLUCK_PAN_MAX ? PLUCK_PAN_MAX : pan);
            } else if (page == PAGE_MUTE && steps[k]) {
                pluck_parts[k].muted = steps[k] < 0;
            } else if (page == PAGE_LEAK && steps[k]) {
                tape_adjust_leak(&tapes[k], steps[k]);
            }
        }
        int32_t mode_step = fm1_enc_take(ENC_ALGORITHM);
        if (editing && macros_dirty) edit_sound(&pluck_parts[armed_tape], steps);
        if (!editing || !(fm1_in.buttons & edit_button)) {
            edit_hold_pending = 0;
        } else if ((b & edit_button) && previous_page == PAGE_EDIT) {
            edit_hold_started = fm1_ms;
            edit_hold_pending = 1;
        }
        if (edit_hold_pending && fm1_ms - edit_hold_started > 1000u) {
            edit_hold_pending = 0;
            randomize_sound(&pluck_parts[armed_tape], &sound_random);
            macros_dirty = 1;
        }
        uint8_t previous_root = scale_root, previous_scale = global_scale;
        uint8_t previous_transpose = transpose_root;
        if (page == PAGE_SCALE && macros_dirty) {
            scale_root = limit_setting(scale_root + steps[0], 0, 11);
            global_scale = limit_setting(global_scale + steps[1], 0, SCALE_COUNT - 1);
        }
        transpose_root = NO_TRANSPOSE_ROOT;
        if (page == PAGE_SCALE && transpose_available && transpose_count)
            transpose_root = held[transpose_order[transpose_count - 1u]] % 12u;
        if (previous_transpose != transpose_root || previous_root != scale_root || previous_scale != global_scale) {
            retune_playing_tapes();
            macros_dirty = 1;
        }
        if (page == PAGE_SCALE && (b & ((1u << BTN_PLAY) | (1u << BTN_REC)))) macros_dirty = 1;
        if (page == PAGE_LFO && macros_dirty) edit_lfo(&pluck_parts[armed_tape].lfo, steps);
        if (page == PAGE_ENV && macros_dirty) edit_env(&pluck_parts[armed_tape].adsr, steps);
        if (page == PAGE_TAPE) edit_tape_lengths(steps, mode_step);
        if (page == PAGE_ARP && macros_dirty) edit_arp(steps);
        if (page == PAGE_FX && macros_dirty) edit_fx(steps);
        if (page == PAGE_HALL && macros_dirty) edit_hall(steps);
        if (page == PAGE_GLO && macros_dirty && edit_global(steps)) {
            dirty = 1;
            t_change = fm1_ms;
        }

        if (snapshot_button(b, rel)) {
            transpose_count = 0;
            macros_dirty = 1;
            for (uint32_t i = 0; i < TAPE_COUNT; i++) tape_drawn[i] = ~0u;
        }
        static const char *snapshot_drawn;
        if (snapshot_message && fm1_ms - snapshot_message_at >= SNAPSHOT_MESSAGE_MS)
            snapshot_message = 0;

        midi_in();
        if (clk_n && fm1_ms - last_clock_ms > 500u) clk_n = clk_bpm = 0;
        service_arp();
        update_fx();

        oct = (int8_t)set.oct;
        base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        for (k = 0; k < PLUCK_VOICES; k++)                /* what sounds, on the keys of this octave */
            if (pluck_voices[k].active && !pluck_voices[k].released && pluck_voices[k].note >= base && pluck_voices[k].note < base + 27u)
                lit |= 1u << (pluck_voices[k].note - base);
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_PLAY, tapes[armed_tape].playing);
        fm1_led_key(BTN_REC, tapes[armed_tape].recording);
        fm1_led_key(BTN_SEQ, page == PAGE_TAPE || page == PAGE_LEAK);
        fm1_led_key(BTN_ARP, page == PAGE_ARP || arp.mode != ARP_OFF);
        fm1_led_key(BTN_FX, page_is_fx(page) || fx.on || hall_settings.on);
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {             /* a pad glows lilac while its key sounds */
            uint32_t w = k < 8u ? k + 8u : k - 8u, on = lit >> WHITE[w] & 1u;
            padled_set(k, on ? 90 : 4, on ? 60 : 2, on ? 128 : 8);
        }
        padled_flush();                         /* (sends only what changed) */
#endif

        fm1_led_key(BTN_EDIT, editing || erasing);
        fm1_led_key(BTN_ENV, page == PAGE_ENV);
        fm1_led_key(BTN_LFO, page == PAGE_LFO);
        fm1_led_key(BTN_SEL, page == PAGE_SCALE);
        fm1_led_key(BTN_GLO, page == PAGE_GLO);
        fm1_led_key(BTN_SAVE, snapshot_flashing && (fm1_ms / 250u % 2u == 0));
        if (showing_boot) {
            if (fm1_ms - boot_started < BOOT_LOGO_MS && !b && !keys && !macros_dirty &&
                previous_armed == armed_tape)
                continue;
            showing_boot = 0;
            previous_page = UINT8_MAX;           /* draw the first menu after the splash */
        }
        idle_ok = 1;
        uint8_t footer_dirty = page != previous_page || snapshot_drawn != snapshot_message;
        snapshot_drawn = snapshot_message;
        if (page != previous_page) {
            lcd_fill(0, 0, 240, 240, C_BG);
            macros_dirty = 1;
            for (uint32_t i = 0; i < TAPE_COUNT; i++) tape_drawn[i] = ~0u;
            ui_divider(40, C_RULE);
            if (!editing && page != PAGE_ENV && page != PAGE_LFO) {
                const char *title = page == PAGE_HOME ? "HOME / VOLUME" :
                                    page == PAGE_PAN ? "HOME / PAN" :
                                    page == PAGE_MUTE ? "HOME / MUTE" :
                                    page == PAGE_TAPE ? "TAPE LOOPS" :
                                    page == PAGE_LEAK ? "MEMORY LEAK" :
                                    page == PAGE_ARP ? "ARP / STRUM" :
                                    page == PAGE_SCALE ? "ALL TAPES SCALE" :
                                    page == PAGE_GLO ? "GLO" :
                                    page == PAGE_FX ? "FX DELAY" : "FX HALL";
                ui_title(title);
            }
            if (page != PAGE_ARP && page != PAGE_GLO) ui_divider(211, C_RULE);
        }
        if (editing) {
            if (page != previous_page || previous_armed != armed_tape) {
                char title[] = "TAPE 1  EDIT";
                title[5] = (char)('1' + armed_tape);
                ui_title(title);
                macros_dirty = 1;
            }
            if (macros_dirty) draw_macros(&pluck_parts[armed_tape]);
        } else if (page == PAGE_SCALE) {
            if (macros_dirty) {
                draw_scale();
                footer_dirty = 1;
            }
        } else if (page == PAGE_GLO) {
            if (macros_dirty) draw_global();
        } else if (page == PAGE_LFO) {
            if (macros_dirty || previous_armed != armed_tape) draw_lfo(&pluck_parts[armed_tape].lfo);
        } else if (page == PAGE_ENV) {
            if (macros_dirty || previous_armed != armed_tape) draw_env(&pluck_parts[armed_tape].adsr);
        } else if (page == PAGE_TAPE) {
            footer_dirty |= erase_drawn != erasing;
            erase_drawn = erasing;
            for (uint32_t i = 0; i < TAPE_COUNT; i++) draw_tape_row(i, &tape_drawn[i]);
        } else if (page == PAGE_LEAK) {
            for (uint32_t i = 0; i < TAPE_COUNT; i++) draw_leak_row(i, &tape_drawn[i]);
        } else if (page == PAGE_ARP) {
            uint32_t external = arp_is_strum(&arp) ? 0 : external_bpm();
            if (macros_dirty || external != external_drawn) {
                external_drawn = external;
                draw_arp(external);
            }
        } else if (page == PAGE_FX) {
            uint32_t tempo = fx_bpm() | (external_bpm() ? 1u << 9 : 0);
            if (macros_dirty || tempo != fx_tempo_drawn) {
                fx_tempo_drawn = tempo;
                draw_fx();
                footer_dirty = 1;
            }
        } else if (page == PAGE_HALL) {
            if (macros_dirty) draw_hall();
        } else {
            if (macros_dirty || previous_armed != armed_tape) draw_home_mixer(page);
        }
        if (footer_dirty) draw_page_footer(page, erasing);
#if !BOARD_SMK37
        if (page != previous_page || battery_drawn != battery_level) {
            draw_battery(battery_level);
            battery_drawn = battery_level;
        }
#endif
        idle_ok = 0;

        if (dirty && !tapes[armed_tape].playing && quiet() && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            dirty = settings_save(&set, sizeof set) != 0;
            t_change = fm1_ms;
        }
    }
}
