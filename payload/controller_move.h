#pragma once
#include <stdint.h>

/*
 * PlayStation Move (CECH-ZCM1/ZCM2) input over Bluetooth through the PS5 /dev/hid
 * driver, used by the wheel driver as a shifter or handbrake lever (see shifter.h).
 *
 * Kernel read buffer: 64 bytes per report, [0..7] timestamp, [8] status,
 * [9..] HID input report 0x01:
 *   [0] report id  [1] Select 0x01, Start 0x08
 *   [2] Triangle 0x10, Circle 0x20, Cross 0x40, Square 0x80
 *   [3] PS 0x01, Move 0x08, T 0x10     [4] sequence (upper nibble)
 *   [5],[6] trigger   [13..24] accel x,y,z (2 samples)   [25..36] gyro x,y,z (2 samples)
 * Sensor words are little endian, offset by 0x8000.
 */

#define MOVE_KERNEL_REPORT_SIZE 64
#define MOVE_HID_OFFSET 9

typedef struct {
    uint8_t select, start, triangle, circle, cross, square, ps, move, t_button;
    uint8_t trigger;              /* 0..255 */
    float accel[3];               /* g, latest sample */
    float gyro[3];                /* rad/s, latest sample */
    float accel1[3], gyro1[3];    /* first (older) sample of the report */
} move_input;

/* Parse one 64-byte kernel report. Returns 1 for an input report, 0 otherwise. */
int move_parse_report(const uint8_t *kernel_report, move_input *out);
