#pragma once
#include <stdint.h>
#include <stddef.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/*
 * Thrustmaster Ferrari 458 Spider Racing Wheel (Xbox One)
 * VID=0x044F, PID=0xB671
 *
 * Endpoints: IN=0x81 OUT=0x01 (GIP protocol, maxpkt=64)
 */

#define WHEEL_VID    0x044Fu
#define WHEEL_PID    0xB671u

#define WHEEL_EP_IN  0x81
#define WHEEL_EP_OUT 0x01

typedef struct {
    uint32_t btn_xbox;
    uint32_t btn_menu;
    uint32_t btn_view;
    uint32_t btn_a;
    uint32_t btn_b;
    uint32_t btn_x;
    uint32_t btn_y;
    uint32_t btn_paddle_r;
    uint32_t btn_paddle_l;
    uint32_t deadzone;
    int      invert_pedals;
    uint32_t steer_min, steer_center, steer_max;
    uint32_t throttle_min, throttle_max, brake_min, brake_max;
    uint32_t sensitivity, curve, smoothing_ms;
} wheel_config_t;

/* Indices for button instrumentation; independent of the configurable mapping. */
enum { WHEEL_BTN_A, WHEEL_BTN_B, WHEEL_BTN_X, WHEEL_BTN_Y, WHEEL_BTN_MENU, WHEEL_BTN_VIEW,
       WHEEL_BTN_UP, WHEEL_BTN_DOWN, WHEEL_BTN_LEFT, WHEEL_BTN_RIGHT,
       WHEEL_BTN_PADDLE_L, WHEEL_BTN_PADDLE_R, WHEEL_BTN_XBOX, WHEEL_BTN_COUNT };

/* What happened to the virtual-pad update that carried a button change. */
enum { WHEEL_SEND_PENDING, WHEEL_SEND_OK, WHEEL_SEND_FAILED, WHEEL_SEND_SKIPPED };

/* Outcome of one USB IN transfer, reported by the reader loop. */
enum { WHEEL_POLL_DATA, WHEEL_POLL_TIMEOUT, WHEEL_POLL_DISCARDED };

typedef struct {
    uint8_t  connected;
    int16_t  steer_deg;    /* -120 a +120 graus */
    uint8_t  throttle_pct; /* 0 a 100 % */
    uint8_t  brake_pct;    /* 0 a 100 % */
    uint8_t  paddle_l;     /* 1 se pressionada */
    uint8_t  paddle_r;     /* 1 se pressionada */
    uint32_t buttons;
    int8_t   gear;         /* -1 = R, 0 = N, 1..8 = 1a..8a */
    uint8_t  btn_a;
    uint8_t  btn_b;
    uint8_t  btn_x;
    uint8_t  btn_y;
    uint8_t  btn_up;
    uint8_t  btn_down;
    uint8_t  btn_left;
    uint8_t  btn_right;
    uint8_t  btn_menu;
    uint8_t  btn_view;
    uint8_t  btn_xbox;
    uint16_t raw_steer, raw_throttle, raw_brake;
    uint32_t age_ms;
    uint8_t fresh;
    /* USB transfers keep completing (with timeouts) while the wheel is idle. */
    uint32_t link_age_ms;
    /* Presses since the last counter reset; lets clients see taps between polls. */
    uint32_t press_count[WHEEL_BTN_COUNT];
    int8_t   last_button;  /* -1 = none */
    uint8_t  last_outcome; /* WHEEL_SEND_* of the last button change */
    uint32_t last_button_age_ms;
} wheel_telemetry_t;

typedef struct {
    uint32_t presses, releases;          /* edges seen in USB reports */
    uint32_t sent_ok, send_failed, not_sent; /* fate of the press edges */
    uint32_t last_hold_ms, min_hold_ms;  /* min 0 = no release seen yet */
} wheel_button_stats_t;

#define WHEEL_EVENT_COUNT 128
typedef struct {
    uint32_t id, t_ms;      /* t_ms counts from the last counter reset */
    uint32_t gap_ms;        /* time since the previous USB input packet */
    uint32_t hold_ms;       /* for releases: how long the button was held */
    uint32_t inject_us;     /* packet parsed -> virtual pad updated */
    uint32_t pad_buttons;   /* converted DualSense mask sent to the PS5 */
    int32_t  inject_result;
    uint16_t pressed, released, held; /* bit n = WHEEL_BTN_n */
    uint8_t  cmd, seq, raw4, raw5, r2, l2, stick_x, seq_lost, outcome;
} wheel_button_event_t;

typedef struct {
    uint32_t now_ms, sessions, event_total, event_count;
    uint64_t usb_polls, usb_timeouts, usb_discarded;
    uint64_t short_packets, other_packets, seq_lost, seq_repeated;
    uint8_t  last_other_cmd, last_other_len;
    wheel_button_stats_t buttons[WHEEL_BTN_COUNT];
    wheel_button_event_t events[WHEEL_EVENT_COUNT]; /* oldest first */
} wheel_button_report_t;

#define WHEEL_PROFILE_COUNT 8
#define WHEEL_PROFILE_NAME 49
typedef struct {
    char name[WHEEL_PROFILE_NAME];
    wheel_config_t config;
} wheel_profile_t;

enum { WHEEL_WAITING, WHEEL_DETECTED, WHEEL_USB_OPEN, WHEEL_HANDSHAKE,
       WHEEL_READY, WHEEL_STREAMING, WHEEL_DISCONNECTED, WHEEL_ERROR };
typedef struct {
    int stage, usb_found, handshake_ok, virtual_ready;
    int32_t last_error, last_injection;
    uint64_t input_packets, usb_errors, injection_attempts, injection_errors;
    uint32_t age_ms, rate_hz;
} wheel_diagnostics_t;

void wheel_config_init_default(void);
void wheel_config_load(void);
void wheel_config_get(wheel_config_t *out);
int wheel_config_set_value(wheel_config_t *cfg, const char *key, const char *value);
int wheel_config_apply(const wheel_config_t *cfg);
void wheel_config_default(wheel_config_t *out);
int wheel_config_validate(const wheel_config_t *cfg);
size_t wheel_config_field_count(void);
const char *wheel_config_field_name(size_t index);
uint32_t wheel_config_field_value(const wheel_config_t *cfg, size_t index);
void wheel_profiles_get(wheel_profile_t out[WHEEL_PROFILE_COUNT]);
void wheel_backup_get(wheel_config_t *cfg, wheel_profile_t profiles[WHEEL_PROFILE_COUNT]);
int wheel_profile_name_valid(const char *name);
int wheel_profile_save(unsigned slot, const char *name, const wheel_config_t *cfg);
int wheel_profile_load(unsigned slot);
int wheel_profile_delete(unsigned slot);
int wheel_backup_apply(const wheel_config_t *cfg, const wheel_profile_t profiles[WHEEL_PROFILE_COUNT]);
int wheel_set_gear(int gear);
/* PS Move shifter: make extra the buttons held on top of the wheel (also applied to
 * later USB reports) and build the pad to resend now. Returns 0 before the wheel
 * has sent a report, so nothing is injected without a wheel. */
int wheel_compose_pad(uint32_t extra, ScePadData *out);
/* Count a shift from the PS Move in the gear indicator, like a paddle press. */
void wheel_virtual_shift(int up);
void wheel_diagnostics_get(wheel_diagnostics_t *out);
void wheel_diagnostics_event(int stage, int32_t error);
void wheel_diagnostics_injection(int32_t result);
void wheel_diagnostics_not_sent(void);
void wheel_diagnostics_usb_poll(int kind);
const char *wheel_button_name(unsigned index);
void wheel_buttons_get(wheel_button_report_t *out);
void wheel_buttons_reset(void);
void wheel_telemetry_get(wheel_telemetry_t *out);
void wheel_reset_state(void);

/* Parse GIP wheel report into ScePadData */
void wheel_parse_input(const uint8_t *buf, ScePadData *o);

/* Handshake initialization sequence */
int wheel_gip_handshake(int fd, struct usb_fs_endpoint *eps);

/* Handle one packet from the wheel */
int wheel_handle_packet(int fd, struct usb_fs_endpoint *eps,
                        const uint8_t *buf, uint32_t len,
                        ScePadData *out_pad);
