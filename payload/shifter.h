#pragma once
#include <stdint.h>

/*
 * PS Move as a sequential shifter for the wheel driver.
 * The Move sits in a stand that locks in levels (HY300 projector tube).
 *
 * Step mode: a shift is a move from one resting level of the stand to another
 * at least on_deg away (forward = up). Tilt is measured from gravity relative
 * to a centre pose (hold Move 1 s).
 * The forward direction defaults to the controller's button face and can be
 * taught by tilting forward and holding Triangle 1 s.
 *
 * Shifts only reach the game when the option is enabled (off by default);
 * the driver then presses the wheel's paddle buttons on the virtual pad.
 */

#define SHIFTER_EVENT_COUNT 32
#define SHIFTER_REST_COUNT 16

enum { SHIFT_NONE = 0, SHIFT_UP = 1, SHIFT_DOWN = 2 };

typedef struct {
    uint64_t at_us;
    uint8_t  kind;           /* SHIFT_UP / SHIFT_DOWN */
    float    peak_deg;       /* largest tilt of that push/pull */
    float    peak_gyro;      /* largest rotation speed, deg/s */
    uint32_t duration_ms;    /* time beyond the release threshold */
} shifter_event;

typedef struct {
    float    deg;            /* stable tilt, averaged */
    uint32_t hits;
} shifter_rest;

typedef struct {
    int      connected;
    uint32_t age_ms;
    float    tilt_deg, side_deg, gyro_dps;
    float    on_deg, off_deg; /* on = smallest move between levels that is a shift */
    float    level_deg;       /* resting level of the last shift */
    int      state;          /* SHIFT_NONE / SHIFT_UP / SHIFT_DOWN while held */
    int      centred, forward_taught;
    uint32_t ups, downs;
    uint8_t  trigger, t_button, move_button, cross, circle, square, triangle;
    shifter_event events[SHIFTER_EVENT_COUNT];
    uint32_t event_count;    /* total, ring holds the latest SHIFTER_EVENT_COUNT */
    shifter_rest rests[SHIFTER_REST_COUNT];
    uint32_t rest_count;
    int      enabled;        /* send shifts to the game */
    uint32_t handbrake;      /* DualSense button for the handbrake (T or lever); 0 = none */
    uint32_t up_button, down_button; /* pressed on a shift; 0 = the wheel's paddle buttons */
    uint32_t sent;           /* shifts pressed on the virtual pad */
    int      fast;           /* option: shift mid-push once the axis is learned */
    int      learned, learn_count; /* rotation axis learned from level-mode shifts */
    int      lever;          /* option: handbrake lever instead of a shifter */
    float    lever_deg;      /* pull back this far to apply it */
    int      lever_pulled;   /* the handbrake is applied now */
    int      lever_hold;     /* Move button that must be held too (SHIFTER_HOLD_*) */
} shifter_status;

typedef struct {
    uint8_t trigger, t_button, move_button, cross, circle, square, triangle;
} shifter_buttons;

void shifter_init(void);
/* accel in g, gyro in rad/s, one IMU sample. Returns SHIFT_UP/SHIFT_DOWN when a shift
 * was just recognised, SHIFT_NONE otherwise. */
int shifter_update(const float accel[3], const float gyro[3], const shifter_buttons *b, uint64_t now_us);
void shifter_disconnected(void);
void shifter_get(shifter_status *out, uint64_t now_us);
void shifter_reset_counts(void);
/* Returns 0, or -1 when off >= on or values are outside 2..45. */
int shifter_set_thresholds(float on_deg, float off_deg);
void shifter_request_centre(void);

/* Options, kept in a small file next to the wheel configuration. */
void shifter_set_options(int enabled, uint32_t handbrake, uint32_t up_button, uint32_t down_button);
void shifter_count_sent(void);
void shifter_set_fast(int fast);
/* Returns -1 when deg is outside 3..40. */
int shifter_set_lever(int lever, float deg);
/* Button that must be held with the pull, so a Move that tips back alone does not
 * apply the handbrake. Move and Triangle are left out: holding them 1 s re-centres
 * and teaches the forward direction. */
enum { SHIFTER_HOLD_NONE, SHIFTER_HOLD_T, SHIFTER_HOLD_CROSS, SHIFTER_HOLD_CIRCLE, SHIFTER_HOLD_SQUARE, SHIFTER_HOLD_COUNT };
void shifter_set_lever_hold(int hold);
/* Whether the chosen button is held in this Move report. */
int shifter_hold_pressed(int hold, const shifter_buttons *b);
int shifter_valid_button(unsigned long v);
/* Test button on /cambio: the next reader cycle sends this shift as if the Move made it. */
int shifter_request_test(int kind);
int shifter_take_test(void);
/* Bluetooth is only scanned for a Move while the option is on or /cambio is open
 * (asked for its state in the last SHIFTER_VIEW_US), so other pads are left alone. */
#define SHIFTER_VIEW_US 30000000u
void shifter_note_viewer(uint64_t now_us);
int shifter_wanted(uint64_t now_us);
/* Clock for every now_us above: monotonic, so a clock change cannot fake a rest. */
uint64_t shifter_now_us(void);
int shifter_options_load(const char *path);   /* missing file keeps the defaults */
int shifter_options_save(const char *path);   /* atomic: temp file + rename */

#ifndef WHEEL_CFG_DIR
#define WHEEL_CFG_DIR "/data/ghostpad"
#endif
#define SHIFTER_OPTIONS_PATH WHEEL_CFG_DIR "/move_shifter.txt"
