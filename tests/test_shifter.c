#include "shifter.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void check(int ok, const char *msg) { printf("%s %s\n", ok ? "PASS" : "FAIL", msg); fails += !ok; }

static uint64_t now = 1000000;
static const shifter_buttons none = { 0 };
static float angle = 0;
static float gyro_sign = 1;   /* -1: the gyro turns the other way (stale learned axis) */

static void sample(float deg, float gx_dps, const shifter_buttons *b) {
    float r = deg / 57.29578f;
    float a[3] = { 0, cosf(r), sinf(r) };   /* gravity "up": Y up, Z = button face */
    float g[3] = { gx_dps / 57.29578f, 0, 0 };
    shifter_update(a, g, b, now);
    now += 5000;
}

/* Tilt the tube to deg over 100 ms (the gyro reports the motion), then leave it there. */
static void hold(float deg, const shifter_buttons *b, int ms) {
    int steps = ms / 5, move = steps < 20 ? steps : 20;
    float from = angle, rate = move ? (deg - from) / (move * 0.005f) : 0;
    for (int i = 1; i <= steps; i++) {
        if (i <= move) sample(from + (deg - from) * i / move, gyro_sign * rate, b);
        else sample(deg, 0, b);
    }
    angle = deg;
}

/* A shove: the accelerometer reads a tilt while the tube is moving, then it is back. */
static void knock(float deg, int ms) {
    /* The rig shakes about another axis than the tube's pivot (sensor X here). */
    float r = deg / 57.29578f, a[3] = { 0, cosf(r), sinf(r) }, g[3] = { 0, 0, 60 / 57.29578f };
    for (int i = 0; i < ms / 5; i++) { shifter_update(a, g, &none, now); now += 5000; }
}

int main(int argc, char **argv) {
    shifter_status st;
    shifter_init();

    hold(0, &none, 600);
    shifter_get(&st, now);
    check(st.connected && st.centred, "centres after the Move is still");
    check(fabsf(st.tilt_deg) < 0.5f && st.ups == 0, "upright reads 0 deg, no shift");

    hold(11.6f, &none, 700);
    shifter_get(&st, now);
    check(st.ups == 1 && st.downs == 0, "settling on the first forward level is one upshift");
    check(fabsf(st.level_deg - 11.6f) < 0.6f, "the level becomes the new reference");
    check(st.events[0].peak_gyro >= 100, "event keeps the speed of the push");

    hold(11.6f, &none, 2000);
    shifter_get(&st, now);
    check(st.ups == 1, "staying on a forward level does not count again");

    hold(0, &none, 700);
    shifter_get(&st, now);
    check(st.ups == 1 && st.downs == 1, "coming back to the middle is one downshift");

    /* Overshoot: the tube settles past the level, then slides back onto it. */
    hold(16, &none, 300);
    hold(11.6f, &none, 300);
    shifter_get(&st, now);
    check(st.ups == 2 && st.downs == 1, "a bounce right after a shift is not a second shift");
    hold(0, &none, 700);
    shifter_get(&st, now);
    check(st.downs == 2 && fabsf(st.level_deg) < 0.6f, "the bounce still moved the level");

    hold(11.6f, &none, 700);
    hold(22, &none, 700);
    shifter_get(&st, now);
    check(st.ups == 4, "two forward levels in a row are two upshifts");

    hold(0, &none, 700);
    shifter_get(&st, now);
    check(st.downs == 3, "a jump of two levels back counts one downshift");

    hold(-12, &none, 700);
    shifter_get(&st, now);
    check(st.downs == 4, "a backward level from the middle is a downshift");
    hold(0, &none, 700);
    shifter_get(&st, now);
    check(st.ups == 5, "back to the middle from behind is an upshift");

    knock(25, 40); knock(-20, 30);
    hold(0, &none, 700);
    shifter_get(&st, now);
    check(st.ups == 5 && st.downs == 4, "knocks that never settle are not shifts");

    hold(3, &none, 700);
    shifter_get(&st, now);
    check(st.ups == 5, "a 3 deg wobble is below the minimum step");
    check(st.rest_count >= 4, "levels are listed as positions");
    hold(0, &none, 2500);

    check(shifter_set_thresholds(5, 6) == -1, "rejects off >= on");
    check(shifter_set_thresholds(9, 3) == 0, "accepts a new minimum step");
    hold(7, &none, 700);
    shifter_get(&st, now);
    check(st.ups == 5, "7 deg is below a 9 deg minimum step");
    hold(0, &none, 2500);

    hold(2, &none, 2500);
    shifter_get(&st, now);
    check(fabsf(st.tilt_deg) < 0.3f, "2 deg drift at rest is re-centred");
    hold(0, &none, 2500);

    /* Re-centre on the leaning pose by holding Move 1 s. */
    shifter_buttons mv = { 0 }; mv.move_button = 1;
    hold(7, &mv, 1300);
    hold(7, &none, 700);
    shifter_get(&st, now);
    check(fabsf(st.tilt_deg) < 0.5f && fabsf(st.level_deg) < 0.5f, "holding Move re-centres the level too");

    shifter_reset_counts();
    shifter_get(&st, now);
    check(st.ups == 0 && st.event_count == 0 && st.rest_count == 0, "reset clears counters");

    /* Teach forward from a sideways tilt: Triangle 1 s while leaning around Z. */
    shifter_init(); angle = 0;
    hold(0, &none, 600);
    shifter_buttons tri = { 0 }; tri.triangle = 1;
    float r = 15 / 57.29578f, a[3] = { sinf(r), cosf(r), 0 }, g[3] = { 0, 0, 0 };
    for (int i = 0; i < 260; i++) { shifter_update(a, g, &tri, now); now += 5000; }
    shifter_get(&st, now);
    check(st.forward_taught && st.tilt_deg > 14 && fabsf(st.side_deg) < 1, "Triangle teaches the forward direction");

    check(shifter_request_test(SHIFT_UP) == 0 && shifter_take_test() == SHIFT_UP && shifter_take_test() == SHIFT_NONE, "a test shift is taken once");
    check(shifter_request_test(7) == -1, "unknown test shift is refused");
    shifter_request_test(SHIFT_DOWN);
    shifter_disconnected();
    check(shifter_take_test() == SHIFT_NONE, "a test pressed while the Move is away is dropped");
    check(shifter_now_us() > 0, "monotonic clock works");
    shifter_set_options(0, 0, 0, 0);
    check(!shifter_wanted(now), "nothing scans Bluetooth while off and the page is closed");
    shifter_note_viewer(now);
    check(shifter_wanted(now + 1000000) && !shifter_wanted(now + SHIFTER_VIEW_US + 1), "an open page scans for a while");
    shifter_set_options(1, 0, 0, 0);
    check(shifter_wanted(now + SHIFTER_VIEW_US + 1), "the option on always scans");
    shifter_set_options(0, 0, 0, 0);

    shifter_disconnected();
    shifter_get(&st, now);
    check(!st.connected, "disconnect is reported");

    /* Fast mode: learn the rotation axis from level shifts, then shift mid-push. */
    shifter_init(); angle = 0; gyro_sign = 1;
    shifter_set_thresholds(3, 1.5f);
    shifter_set_fast(1);
    hold(0, &none, 600);
    hold(8, &none, 700); hold(0, &none, 700); hold(8, &none, 700);
    shifter_get(&st, now);
    check(st.ups == 2 && st.downs == 1 && st.learned && st.fast, "three level shifts teach the axis");
    {   /* The shift arrives while the tube is still moving, not after it stops. */
        float from = angle, to = 0; int fired_at = -1;
        for (int i = 1; i <= 20; i++) {
            int k = 0;
            float r = (to - from) / 0.1f;
            float d = from + (to - from) * i / 20;
            float rad = d / 57.29578f, a[3] = { 0, cosf(rad), sinf(rad) }, g[3] = { r / 57.29578f, 0, 0 };
            k = shifter_update(a, g, &none, now); now += 5000;
            if (k == SHIFT_DOWN && fired_at < 0) fired_at = i;
        }
        angle = to;
        check(fired_at > 0 && fired_at <= 6, "fast mode fires within ~30 ms of the push");
    }
    hold(0, &none, 700);
    shifter_get(&st, now);
    check(st.downs == 2 && st.ups == 2, "the level at the end of a fast shift does not count again");
    hold(12, &none, 120); hold(8, &none, 700);   /* overshoot, then slide back onto the level */
    shifter_get(&st, now);
    check(st.ups == 3, "a bounce before the tube stops is not a second fast shift");
    knock(30, 40);
    hold(8, &none, 700);
    shifter_get(&st, now);
    check(st.ups == 3 && st.downs == 2, "a knock without rotation along the axis is not a shift");
    gyro_sign = -1;   /* the Move was turned in its holder: the learned axis is now wrong */
    hold(0, &none, 700); hold(8, &none, 700);
    shifter_get(&st, now);
    check(!st.learned, "two shifts in the wrong direction unlearn the axis");
    gyro_sign = 1;
    shifter_set_fast(0);

    /* Handbrake lever: pulled back applies it, forward again releases it; no shifts. */
    shifter_init(); angle = 0;
    shifter_set_thresholds(3, 1.5f);
    check(shifter_set_lever(1, 2) == -1 && shifter_set_lever(1, 41) == -1, "lever angle outside 3..40 is refused");
    check(shifter_set_lever(1, 8) == 0, "lever mode is accepted");
    hold(0, &none, 600);
    hold(-12, &none, 300);
    shifter_get(&st, now);
    check(st.lever && st.lever_pulled && st.ups == 0 && st.downs == 0, "pulling back applies the handbrake and is not a shift");
    hold(-6, &none, 300);
    shifter_get(&st, now);
    check(st.lever_pulled, "easing off a little keeps it applied");
    hold(-2, &none, 300);
    shifter_get(&st, now);
    check(!st.lever_pulled, "back near the middle releases it");
    hold(10, &none, 700);
    shifter_get(&st, now);
    check(!st.lever_pulled && st.ups == 0, "pushing forward neither applies it nor shifts");
    shifter_set_lever(0, 8);
    shifter_get(&st, now);
    check(!st.lever && !st.lever_pulled, "lever mode can be turned off");
    shifter_buttons held = { 0 };
    check(shifter_hold_pressed(SHIFTER_HOLD_NONE, &held), "no hold button: the pull alone applies it");
    check(!shifter_hold_pressed(SHIFTER_HOLD_T, &held), "T released: not held");
    held.trigger = 200;
    check(shifter_hold_pressed(SHIFTER_HOLD_T, &held) && !shifter_hold_pressed(SHIFTER_HOLD_CROSS, &held), "T pulled counts only for T");
    held.circle = 1;
    check(shifter_hold_pressed(SHIFTER_HOLD_CIRCLE, &held), "circle held");
    shifter_set_lever_hold(9);
    shifter_get(&st, now);
    check(st.lever_hold == SHIFTER_HOLD_NONE, "unknown hold button falls back to none");

    if (argc > 1) {
        char path[512];
        snprintf(path, sizeof(path), "%s/move_shifter.txt", argv[1]);
        check(shifter_options_load(path) == 0, "missing options file keeps defaults");
        shifter_get(&st, now);
        check(!st.enabled && st.handbrake == 0, "shifter is off by default");
        shifter_set_options(1, 0x4000, 0x1000, 0x2000);
        shifter_set_thresholds(8, 4);
        check(shifter_options_save(path) == 0, "options are saved");
        shifter_set_options(0, 0, 0, 0);
        shifter_set_thresholds(6, 3);
        shifter_options_load(path);
        shifter_get(&st, now);
        check(st.enabled && st.handbrake == 0x4000 && st.up_button == 0x1000 && st.down_button == 0x2000 && st.on_deg == 8.0f, "options load back");
        shifter_init();
        shifter_get(&st, now);
        check(st.enabled && st.handbrake == 0x4000, "a reconnect keeps the options");
        FILE *f = fopen(path, "w");
        fputs("enabled=1\nhandbrake=12\nup=6\ndown=4\nstep=99\n", f);
        fclose(f);
        shifter_options_load(path);
        shifter_get(&st, now);
        check(st.handbrake == 0 && st.up_button == 0 && st.down_button == 4 && st.on_deg == 6.0f, "invalid buttons and step fall back");
    }

    printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
