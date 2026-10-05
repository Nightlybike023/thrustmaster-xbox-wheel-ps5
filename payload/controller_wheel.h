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
} wheel_telemetry_t;

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
void wheel_diagnostics_get(wheel_diagnostics_t *out);
void wheel_diagnostics_event(int stage, int32_t error);
void wheel_diagnostics_injection(int32_t result);
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
