/* SPDX-License-Identifier: GPL-3.0-or-later
 * controller_move.c — PS Move input report parsing */
#include "controller_move.h"
#include <string.h>

#define MOVE_ONE_G      4096.0f
#define MOVE_GYRO_SCALE 0.0010908f /* rad/s per count (~16 LSB per deg/s) */

static float sensor_word(const uint8_t *p) {
    return (float)((int32_t)(p[0] | (p[1] << 8)) - 0x8000);
}

int move_parse_report(const uint8_t *kr, move_input *o) {
    const uint8_t *r = kr + MOVE_HID_OFFSET;
    memset(o, 0, sizeof(*o));
    /* The PS5 driver does not keep the HID report id (0x30 observed, 0x01 on PC);
     * the Move's fixed 7F 7F 7F 7F bytes identify the input report instead. */
    if (r[7] != 0x7f || r[8] != 0x7f || r[9] != 0x7f || r[10] != 0x7f) return 0;
    o->select   = !!(r[1] & 0x01);
    o->start    = !!(r[1] & 0x08);
    o->triangle = !!(r[2] & 0x10);
    o->circle   = !!(r[2] & 0x20);
    o->cross    = !!(r[2] & 0x40);
    o->square   = !!(r[2] & 0x80);
    o->ps       = !!(r[3] & 0x01);
    o->move     = !!(r[3] & 0x08);
    o->t_button = !!(r[3] & 0x10);
    o->trigger  = (uint8_t)((r[5] + r[6]) / 2);
    for (int i = 0; i < 3; i++) {
        o->accel1[i] = sensor_word(r + 13 + 2 * i) / MOVE_ONE_G;
        o->accel[i]  = sensor_word(r + 19 + 2 * i) / MOVE_ONE_G;
        o->gyro1[i]  = sensor_word(r + 25 + 2 * i) * MOVE_GYRO_SCALE;
        o->gyro[i]   = sensor_word(r + 31 + 2 * i) * MOVE_GYRO_SCALE;
    }
    return 1;
}
