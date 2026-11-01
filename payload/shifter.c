/*
 * shifter.c — PS Move as a sequential shifter or handbrake lever (see shifter.h)
 */
#include "shifter.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RAD2DEG 57.29578f
#define STILL_DPS 15.0f
#define STILL_US 180000u    /* 0.3 s felt laggy in WRC, 0.12 s doubled shifts */
#define SHIFT_LOCKOUT_US 350000u /* the tube bounces on a level: no second shift this soon */
#define HOLD_US 1000000u
#define REST_MERGE_DEG 2.5f
#define JERK_G 0.15f
#define AUTO_CENTRE_US 2000000u
#define AUTO_CENTRE_DEG 4.0f
#define FILTER_ALPHA 0.11f   /* ~50 ms time constant at ~174 samples/s */
#define BIAS_GAIN 0.01f
/* Fast mode: shift on the rotation itself, mid-push, then wait for the tube to stop. */
#define FAST_DPS 40.0f          /* rotation speed along the learned axis that starts a shift */
#define FAST_MIN_DEG 1.5f       /* and at least this much rotation in that push */
#define FAST_REARM_US 90000u    /* still this long before the next shift can fire */
#define LEARN_SHIFTS 3          /* level-mode shifts needed to learn the rotation axis */
#define LEARN_MIN_RAD 0.035f    /* ~2 deg of rotation for a push to teach anything */
#define LEVER_DEFAULT_DEG 8.0f  /* handbrake lever: pulled back past this applies it */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int connected;
    uint64_t last_us;
    float filt[3];
    int have_filt;
    float bias[3];               /* gyro offset, rad/s */
    float centre[3], fwd[3], side[3];
    int centred, forward_taught, centre_pending;
    float tilt, side_deg, gyro_dps;
    float on_deg, off_deg;
    int state;
    uint64_t state_since;
    float peak, peak_gyro;
    uint32_t ups, downs;
    shifter_buttons buttons;
    shifter_event events[SHIFTER_EVENT_COUNT];
    uint32_t event_count;
    shifter_rest rests[SHIFTER_REST_COUNT];
    uint32_t rest_count;
    uint64_t still_since;
    float still_tilt;
    float level;                 /* tilt of the resting level the last shift ended on */
    uint64_t moving_since, last_shift_us;
    float recent_gyro;           /* fastest rotation since the tube last settled */
    int rest_recorded;
    uint64_t move_since, tri_since;
    int enabled;                 /* options survive shifter_init() */
    uint32_t handbrake, up_button, down_button, sent;
    int fast;                    /* option: shift mid-push (survives shifter_init() too) */
    int lever;                   /* option: the Move is a handbrake lever, not a shifter */
    float lever_deg;             /* pull back this far to apply the handbrake */
    int lever_pulled;
    int lever_hold;              /* option, survives shifter_init() */
    /* The gyro's axes do not match the accelerometer's on the Move, so the pivot axis
     * is learned in gyro coordinates from level-mode shifts: +axis = towards "up". */
    float rot[3];                /* rotation of the current push, rad */
    float learn[3], axis[3];
    int learn_count, learned, mismatches;
    int armed, fast_fired;       /* fast_fired: kind fired during the current push */
    float swing;                 /* rotation along the axis in the current push, deg */
} s;

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static int normalize3(float v[3]) {
    float n = sqrtf(dot3(v, v));
    if (n < 1e-4f) return 0;
    v[0] /= n; v[1] /= n; v[2] /= n;
    return 1;
}
static void cross3(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

/* Component of v perpendicular to the centre direction. */
static int perpendicular(const float v[3], float o[3]) {
    float d = dot3(v, s.centre);
    for (int i = 0; i < 3; i++) o[i] = v[i] - d * s.centre[i];
    return normalize3(o);
}

static void set_forward(const float f[3]) {
    float p[3];
    if (!perpendicular(f, p)) {
        static const float x[3] = { 1, 0, 0 };
        perpendicular(x, p);
    }
    memcpy(s.fwd, p, sizeof(p));
    cross3(s.centre, s.fwd, s.side);
}

static void set_centre_locked(void) {
    float c[3] = { s.filt[0], s.filt[1], s.filt[2] };
    if (!normalize3(c)) return;
    memcpy(s.centre, c, sizeof(c));
    /* Default forward: the button face (sensor Z), kept if taught before. */
    static const float z[3] = { 0, 0, 1 };
    set_forward(s.forward_taught ? s.fwd : z);
    s.centred = 1;
    s.level = 0;
    s.state = SHIFT_NONE;
}

void shifter_init(void) {
    pthread_mutex_lock(&g_lock);
    int enabled = s.enabled;
    uint32_t handbrake = s.handbrake, up = s.up_button, down = s.down_button;
    int fast = s.fast, lever = s.lever, lever_hold = s.lever_hold;
    float lever_deg = s.lever_deg;
    memset(&s, 0, sizeof(s));
    s.lever_hold = lever_hold;
    s.fast = fast;
    s.lever = lever;
    s.lever_deg = lever_deg > 0 ? lever_deg : LEVER_DEFAULT_DEG;
    s.enabled = enabled;
    s.handbrake = handbrake;
    s.up_button = up;
    s.down_button = down;
    s.on_deg = 6.0f;
    s.off_deg = 3.0f;
    pthread_mutex_unlock(&g_lock);
}

static void add_event_locked(uint8_t kind, uint64_t now) {
    shifter_event *e = &s.events[s.event_count % SHIFTER_EVENT_COUNT];
    e->at_us = now;
    e->kind = kind;
    e->peak_deg = s.peak;
    e->peak_gyro = s.peak_gyro;
    e->duration_ms = 0;
    s.event_count++;
    if (kind == SHIFT_UP) s.ups++; else s.downs++;
}

static void add_rest_locked(float deg) {
    for (uint32_t i = 0; i < s.rest_count; i++) {
        shifter_rest *r = &s.rests[i];
        if (fabsf(r->deg - deg) <= REST_MERGE_DEG) {
            r->deg = (r->deg * (float)r->hits + deg) / (float)(r->hits + 1);
            r->hits++;
            return;
        }
    }
    if (s.rest_count < SHIFTER_REST_COUNT) {
        s.rests[s.rest_count].deg = deg;
        s.rests[s.rest_count].hits = 1;
        s.rest_count++;
    }
}

static int held_for(uint8_t pressed, uint64_t *since, uint64_t now) {
    if (!pressed) { *since = 0; return 0; }
    if (!*since) { *since = now; return 0; }
    if (*since != 1 && now - *since >= HOLD_US) { *since = 1; return 1; } /* 1 = already fired */
    return 0;
}

int shifter_update(const float accel[3], const float gyro[3], const shifter_buttons *b, uint64_t now) {
    pthread_mutex_lock(&g_lock);
    int shifted = SHIFT_NONE;
    float dt = s.have_filt && now > s.last_us ? (float)(now - s.last_us) / 1e6f : 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    s.connected = 1;
    s.last_us = now;
    s.buttons = *b;
    float g = sqrtf(dot3(accel, accel));
    if (!s.have_filt) {
        memcpy(s.filt, accel, sizeof(s.filt));
        if (!normalize3(s.filt)) { pthread_mutex_unlock(&g_lock); return SHIFT_NONE; }
        s.have_filt = 1;
    }
    /* Tilt comes from the accelerometer only. Integrating the gyro into the angle made
     * pushes swing to ±50° (its axes do not match the accelerometer's), so the gyro is
     * used for speed, stillness and the learned fast-mode axis instead.
     * Readings away from 1 g are knocks or jerks: keep the last angle. */
    float w[3] = { gyro[0] - s.bias[0], gyro[1] - s.bias[1], gyro[2] - s.bias[2] };
    if (fabsf(g - 1.0f) <= JERK_G)
        for (int i = 0; i < 3; i++) s.filt[i] += FILTER_ALPHA * (accel[i] / g - s.filt[i]);
    normalize3(s.filt);
    s.gyro_dps = sqrtf(dot3(w, w)) * RAD2DEG;
    if (s.gyro_dps > s.recent_gyro) s.recent_gyro = s.gyro_dps;

    int still = s.gyro_dps < STILL_DPS;
    if (!still) { s.still_since = 0; s.rest_recorded = 0; }
    else if (!s.still_since) s.still_since = now;
    int settled = still && now - s.still_since >= STILL_US;
    /* At rest the gyro should read zero: learn its offset so the angle does not creep. */
    if (settled) for (int i = 0; i < 3; i++) s.bias[i] += BIAS_GAIN * (gyro[i] - s.bias[i]);

    if (held_for(b->move_button, &s.move_since, now)) s.centre_pending = 1;
    if ((s.centre_pending || !s.centred) && settled) { set_centre_locked(); s.centre_pending = 0; }
    if (!s.centred) { pthread_mutex_unlock(&g_lock); return SHIFT_NONE; }

    float a[3] = { s.filt[0], s.filt[1], s.filt[2] };
    float up = dot3(a, s.centre);
    s.tilt = atan2f(dot3(a, s.fwd), up) * RAD2DEG;
    s.side_deg = atan2f(dot3(a, s.side), up) * RAD2DEG;

    if (held_for(b->triangle, &s.tri_since, now) && fabsf(s.tilt) + fabsf(s.side_deg) > 5.0f) {
        float f[3]; memcpy(f, a, sizeof(f));
        set_forward(f);
        s.forward_taught = 1;
        s.tilt = atan2f(dot3(a, s.fwd), up) * RAD2DEG;
        s.side_deg = atan2f(dot3(a, s.side), up) * RAD2DEG;
        s.state = SHIFT_NONE;
    }

    /* A slow lean can stay under the gyro limit: a moved angle also ends the rest. */
    if (s.still_since && fabsf(s.tilt - s.still_tilt) > 1.5f) { s.still_since = now; s.rest_recorded = 0; settled = 0; }
    if (s.still_since == now) s.still_tilt = s.tilt;
    if (!still && !s.moving_since) s.moving_since = now;
    if (!still) for (int i = 0; i < 3; i++) s.rot[i] += w[i] * dt;

    /* Handbrake lever: pulled back past lever_deg applies it, back near the middle
     * (half way) releases it, so a shaking hand does not flicker the button. */
    if (s.lever) {
        if (!s.lever_pulled && s.tilt <= -s.lever_deg) s.lever_pulled = 1;
        else if (s.lever_pulled && s.tilt > -s.lever_deg * 0.5f) s.lever_pulled = 0;
    } else s.lever_pulled = 0;

    /* Fast mode: fire as soon as the push is clearly under way. The bounce on the level
     * happens before the tube stops, so it cannot fire again until it has rested. */
    int fast_active = s.fast && s.learned && !s.lever;
    if (still) {
        s.swing = 0;
        if (s.still_since && now - s.still_since >= FAST_REARM_US) s.armed = 1;
    } else {
        float rate = dot3(w, s.axis) * RAD2DEG;
        s.swing += rate * dt;
        float need = s.on_deg * 0.6f > FAST_MIN_DEG ? s.on_deg * 0.6f : FAST_MIN_DEG;
        if (fast_active && s.armed && fabsf(rate) >= FAST_DPS && fabsf(s.swing) >= need) {
            shifted = s.swing > 0 ? SHIFT_UP : SHIFT_DOWN;
            s.armed = 0;
            s.fast_fired = shifted;
            s.last_shift_us = now;
            s.peak = s.tilt;
            s.peak_gyro = s.recent_gyro;
            add_event_locked((uint8_t)shifted, now);
            shifter_event *e = &s.events[(s.event_count - 1) % SHIFTER_EVENT_COUNT];
            e->duration_ms = s.moving_since ? (uint32_t)((now - s.moving_since) / 1000u) : 0;
        }
    }
    s.state = s.still_since ? SHIFT_NONE : s.tilt > s.level ? SHIFT_UP : SHIFT_DOWN;

    /* Step mode: the HY300 stand locks in levels and does not spring back, so a shift is
     * a move from one resting level to another, not a distance from the middle. Knocks
     * and swings on the way never settle, so they are not counted. */
    if (settled && !s.rest_recorded) {
        float step = s.tilt - s.level;
        int step_kind = fabsf(step) >= s.on_deg ? (step > 0 ? SHIFT_UP : SHIFT_DOWN) : SHIFT_NONE;
        if (s.lever) {
            s.level = s.tilt;   /* no shifts while the Move is the handbrake */
        } else if (s.fast_fired) {
            /* Check the fast shift against where the tube really ended up. Two wrong
             * directions in a row mean the axis is stale: learn it again. */
            if (step_kind && step_kind != s.fast_fired) {
                if (++s.mismatches >= 2) { s.learned = 0; s.learn_count = 0; memset(s.learn, 0, sizeof(s.learn)); s.mismatches = 0; }
            } else if (step_kind) s.mismatches = 0;
            s.level = s.tilt;
        } else if (fabsf(step) >= s.on_deg && s.last_shift_us && now - s.last_shift_us < SHIFT_LOCKOUT_US) {
            s.level = s.tilt;   /* a bounce: follow the level without shifting */
        } else if (fabsf(step) >= s.on_deg) {
            s.last_shift_us = now;
            s.peak = s.tilt;
            s.peak_gyro = s.recent_gyro;
            shifted = step > 0 ? SHIFT_UP : SHIFT_DOWN;
            add_event_locked((uint8_t)shifted, now);
            shifter_event *e = &s.events[(s.event_count - 1) % SHIFTER_EVENT_COUNT];
            e->duration_ms = s.moving_since ? (uint32_t)((now - s.moving_since) / 1000u) : 0;
            s.level = s.tilt;
            /* Each level-mode shift teaches which way the gyro turns for "up". */
            float n = sqrtf(dot3(s.rot, s.rot));
            if (!s.learned && n >= LEARN_MIN_RAD) {
                float sign = shifted == SHIFT_UP ? 1.0f : -1.0f;
                for (int i = 0; i < 3; i++) s.learn[i] += sign * s.rot[i] / n;
                if (++s.learn_count >= LEARN_SHIFTS) {
                    /* Consistent pushes add up; mixed ones cancel out and keep learning. */
                    if (sqrtf(dot3(s.learn, s.learn)) >= 0.8f * (float)s.learn_count) {
                        memcpy(s.axis, s.learn, sizeof(s.axis));
                        normalize3(s.axis);
                        s.learned = 1;
                        s.armed = 0;
                    } else if (s.learn_count >= 3 * LEARN_SHIFTS) {
                        s.learn_count = 0; memset(s.learn, 0, sizeof(s.learn));
                    }
                }
            }
        }
        add_rest_locked(s.tilt);
        s.rest_recorded = 1;
        s.moving_since = 0;
        s.recent_gyro = 0;
        s.fast_fired = SHIFT_NONE;
        memset(s.rot, 0, sizeof(s.rot));
    }
    /* Sensor drift: resting near the middle for 2 s becomes the new centre. */
    if (s.state == SHIFT_NONE && still && now - s.still_since >= AUTO_CENTRE_US &&
        fabsf(s.tilt) < AUTO_CENTRE_DEG && fabsf(s.side_deg) < AUTO_CENTRE_DEG &&
        (fabsf(s.tilt) > 0.3f || fabsf(s.side_deg) > 0.3f)) {
        set_centre_locked();
        s.tilt = s.side_deg = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return shifted;
}

/* A shift requested from the web page, sent by the reader thread like a real one. */
static int g_test_kind;
static uint64_t g_viewed_us;   /* last time /cambio asked for the live state */

void shifter_disconnected(void) {
    pthread_mutex_lock(&g_lock);
    g_test_kind = SHIFT_NONE;  /* a test pressed while the Move was away must not fire later */
    s.connected = 0;
    s.have_filt = 0;
    s.lever_pulled = 0;
    s.state = SHIFT_NONE;
    s.still_since = 0;
    pthread_mutex_unlock(&g_lock);
}

void shifter_get(shifter_status *o, uint64_t now) {
    memset(o, 0, sizeof(*o));
    pthread_mutex_lock(&g_lock);
    o->connected = s.connected;
    o->age_ms = s.last_us && now >= s.last_us ? (uint32_t)((now - s.last_us) / 1000u) : 0;
    o->tilt_deg = s.tilt; o->side_deg = s.side_deg; o->gyro_dps = s.gyro_dps;
    o->on_deg = s.on_deg; o->off_deg = s.off_deg; o->level_deg = s.level;
    o->state = s.state; o->centred = s.centred; o->forward_taught = s.forward_taught;
    o->ups = s.ups; o->downs = s.downs;
    o->trigger = s.buttons.trigger; o->t_button = s.buttons.t_button; o->move_button = s.buttons.move_button;
    o->cross = s.buttons.cross; o->circle = s.buttons.circle; o->square = s.buttons.square; o->triangle = s.buttons.triangle;
    memcpy(o->events, s.events, sizeof(o->events));
    o->event_count = s.event_count;
    memcpy(o->rests, s.rests, sizeof(o->rests));
    o->rest_count = s.rest_count;
    o->enabled = s.enabled; o->handbrake = s.handbrake; o->sent = s.sent;
    o->up_button = s.up_button; o->down_button = s.down_button;
    o->fast = s.fast; o->learned = s.learned; o->learn_count = s.learn_count;
    o->lever = s.lever; o->lever_deg = s.lever_deg; o->lever_pulled = s.lever_pulled; o->lever_hold = s.lever_hold;
    pthread_mutex_unlock(&g_lock);
}

void shifter_reset_counts(void) {
    pthread_mutex_lock(&g_lock);
    s.ups = s.downs = 0;
    s.event_count = 0;
    s.rest_count = 0;
    memset(s.events, 0, sizeof(s.events));
    memset(s.rests, 0, sizeof(s.rests));
    pthread_mutex_unlock(&g_lock);
}

int shifter_set_thresholds(float on_deg, float off_deg) {
    if (!(on_deg >= 2.0f && on_deg <= 45.0f && off_deg >= 1.0f && off_deg < on_deg)) return -1;
    pthread_mutex_lock(&g_lock);
    s.on_deg = on_deg;
    s.off_deg = off_deg;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void shifter_request_centre(void) {
    pthread_mutex_lock(&g_lock);
    s.centre_pending = 1;
    pthread_mutex_unlock(&g_lock);
}

void shifter_set_options(int enabled, uint32_t handbrake, uint32_t up_button, uint32_t down_button) {
    pthread_mutex_lock(&g_lock);
    s.enabled = enabled ? 1 : 0;
    s.handbrake = handbrake;
    s.up_button = up_button;
    s.down_button = down_button;
    pthread_mutex_unlock(&g_lock);
}

/* None, or one DualSense button (same rule as the wheel's button fields). */
int shifter_valid_button(unsigned long v) {
    return v <= 0x001fffffu && !(v & (v - 1u));
}

void shifter_set_fast(int fast) {
    pthread_mutex_lock(&g_lock);
    s.fast = fast ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
}

int shifter_set_lever(int lever, float deg) {
    if (!(deg >= 3.0f && deg <= 40.0f)) return -1;
    pthread_mutex_lock(&g_lock);
    s.lever = lever ? 1 : 0;
    s.lever_deg = deg;
    if (!s.lever) s.lever_pulled = 0;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void shifter_set_lever_hold(int hold) {
    pthread_mutex_lock(&g_lock);
    s.lever_hold = hold >= 0 && hold < SHIFTER_HOLD_COUNT ? hold : SHIFTER_HOLD_NONE;
    pthread_mutex_unlock(&g_lock);
}

int shifter_hold_pressed(int hold, const shifter_buttons *b) {
    switch (hold) {
    case SHIFTER_HOLD_T: return b->trigger > 100;
    case SHIFTER_HOLD_CROSS: return b->cross;
    case SHIFTER_HOLD_CIRCLE: return b->circle;
    case SHIFTER_HOLD_SQUARE: return b->square;
    default: return 1;
    }
}

void shifter_count_sent(void) {
    pthread_mutex_lock(&g_lock);
    s.sent++;
    pthread_mutex_unlock(&g_lock);
}

int shifter_options_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64];
    int enabled = 0;
    unsigned long handbrake = 0, up = 0, down = 0;
    float step = 6.0f;
    int fast = 0, lever = 0, lever_hold = 0;
    float lever_deg = LEVER_DEFAULT_DEG;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "enabled=", 8)) enabled = atoi(line + 8) == 1;
        else if (!strncmp(line, "handbrake=", 10)) handbrake = strtoul(line + 10, NULL, 10);
        else if (!strncmp(line, "up=", 3)) up = strtoul(line + 3, NULL, 10);
        else if (!strncmp(line, "down=", 5)) down = strtoul(line + 5, NULL, 10);
        else if (!strncmp(line, "fast=", 5)) fast = atoi(line + 5) == 1;
        else if (!strncmp(line, "lever=", 6)) lever = atoi(line + 6) == 1;
        else if (!strncmp(line, "lever_deg=", 10)) lever_deg = strtof(line + 10, NULL);
        else if (!strncmp(line, "lever_hold=", 11)) lever_hold = atoi(line + 11);
        else if (!strncmp(line, "step=", 5)) step = strtof(line + 5, NULL);
    }
    fclose(f);
    /* A single DualSense button or none; anything else falls back to none. */
    if (!shifter_valid_button(handbrake)) handbrake = 0;
    if (!shifter_valid_button(up)) up = 0;
    if (!shifter_valid_button(down)) down = 0;
    shifter_set_options(enabled, (uint32_t)handbrake, (uint32_t)up, (uint32_t)down);
    shifter_set_fast(fast);
    if (shifter_set_lever(lever, lever_deg) != 0) shifter_set_lever(lever, LEVER_DEFAULT_DEG);
    shifter_set_lever_hold(lever_hold);
    if (shifter_set_thresholds(step, step / 2.0f) != 0) shifter_set_thresholds(6.0f, 3.0f);
    return 0;
}

int shifter_options_save(const char *path) {
    char temp[256];
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return -1;
    pthread_mutex_lock(&g_lock);
    int enabled = s.enabled;
    uint32_t handbrake = s.handbrake, up = s.up_button, down = s.down_button;
    int fast = s.fast, lever = s.lever, lever_hold = s.lever_hold;
    float step = s.on_deg, lever_deg = s.lever_deg;
    pthread_mutex_unlock(&g_lock);
    FILE *f = fopen(temp, "w");
    if (!f) return -1;
    int bad = fprintf(f, "enabled=%d\nstep=%.1f\nhandbrake=%u\nup=%u\ndown=%u\nfast=%d\nlever=%d\nlever_deg=%.1f\nlever_hold=%d\n", enabled, step, handbrake, up, down, fast, lever, lever_deg, lever_hold) < 0;
    bad |= fflush(f) != 0 || fsync(fileno(f)) != 0;
    bad |= fclose(f) != 0;
    if (bad || rename(temp, path) != 0) { unlink(temp); return -1; }
    return 0;
}

void shifter_note_viewer(uint64_t now) {
    pthread_mutex_lock(&g_lock);
    g_viewed_us = now;
    pthread_mutex_unlock(&g_lock);
}

int shifter_wanted(uint64_t now) {
    pthread_mutex_lock(&g_lock);
    int wanted = s.enabled || (g_viewed_us && now - g_viewed_us < SHIFTER_VIEW_US);
    pthread_mutex_unlock(&g_lock);
    return wanted;
}

int shifter_request_test(int kind) {
    if (kind != SHIFT_UP && kind != SHIFT_DOWN) return -1;
    pthread_mutex_lock(&g_lock);
    g_test_kind = kind;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int shifter_take_test(void) {
    pthread_mutex_lock(&g_lock);
    int kind = g_test_kind;
    g_test_kind = SHIFT_NONE;
    pthread_mutex_unlock(&g_lock);
    return kind;
}

uint64_t shifter_now_us(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
