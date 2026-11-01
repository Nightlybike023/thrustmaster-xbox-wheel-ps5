#include "controller_wheel.h"
#include "web_server.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>

static int send_result, send_count;
int usb_send_out(int fd, struct usb_fs_endpoint *ep, const uint8_t *data,
                 uint32_t len, const char *tag) {
    (void)fd; (void)ep; (void)data; (void)len; (void)tag;
    send_count++;
    return send_result;
}

static uint8_t input[12] = {0x20, 0, 1, 8, 0, 0, 0, 128, 0, 0, 0, 0};
static ScePadData report(const uint8_t *packet, size_t len) {
    ScePadData out = {0};
    struct usb_fs_endpoint eps[2] = {0};
    assert(wheel_handle_packet(-1, eps, packet, (uint32_t)len, &out) == 1);
    return out;
}

static void *reader(void *unused) {
    (void)unused;
    for (int i = 0; i < 5000; i++) {
        ScePadData out = report(input, sizeof(input));
        assert(out.connected && out.leftStick.x == 128);
    }
    return NULL;
}

int main(int argc, char **argv) {
    wheel_config_init_default();
    wheel_reset_state();
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--serve")) {
        if (argc == 4) {
            /* Worst case for /api/buttons: a full ring where every button changes. */
            if (strcmp(argv[3], "--full-buttons")) return 1;
            uint8_t all[12] = {0x20, 0, 1, 8, 0xfc, 0x3f, 0, 128, 0, 0, 0, 0};
            struct usb_fs_endpoint fill_eps[2] = {0};
            ScePadData pad;
            for (int i = 0; i < WHEEL_EVENT_COUNT + 10; i++) {
                all[2] = (uint8_t)(i % 255 + 1); all[4] = i % 2 ? 0 : 0xfc; all[5] = i % 2 ? 0 : 0x3f;
                wheel_handle_packet(-1, fill_eps, all, sizeof(all), &pad);
                wheel_diagnostics_injection(i % 3 ? 0 : -2147483647 - 1);
            }
        }
        if (web_server_start(atoi(argv[2])) != 0) return 2;
        for (;;) pause();
    }
    struct usb_fs_endpoint eps[2] = {0};
    ScePadData out = {0};
    /* Do not read buffers shorter than a command/header or full input report. */
    assert(wheel_handle_packet(-1, eps, NULL, 0, &out) == 0);
    for (size_t n = 0; n < sizeof(input); n++) {
        uint8_t *short_packet = malloc(n ? n : 1);
        memcpy(short_packet, input, n);
        assert(wheel_handle_packet(-1, eps, short_packet, (uint32_t)n, &out) == 0);
        free(short_packet);
    }
    uint8_t guide[5] = {0x07, 0, 1, 1, 1};
    assert(wheel_handle_packet(-1, eps, guide, 4, &out) == 0);
    assert(wheel_handle_packet(-1, eps, guide, 5, &out) == 0);
    out = report(input, sizeof(input));
    assert(out.leftStick.x == 128 && out.analogButtons.r2 == 0);
    input[6] = input[7] = 0;
    assert(report(input, sizeof(input)).leftStick.x == 0);
    input[6] = input[7] = 255;
    assert(report(input, sizeof(input)).leftStick.x == 255);
    input[6] = 0; input[7] = 128;
    input[8] = 0xe8; input[9] = 3;
    input[7] = 192; /* Guide must preserve off-center steering as well. */
    for (int i = 0; i < 6; i++) out = report(input, sizeof(input));
    assert(out.analogButtons.r2 == 255 && !(out.buttons & SCE_PAD_BUTTON_PS));
    input[4] = 2; /* Pedal-associated bit must never activate PS. */
    out = report(input, sizeof(input));
    assert(!(out.buttons & SCE_PAD_BUTTON_PS));
    out = report(guide, sizeof(guide));
    assert(out.analogButtons.r2 == 255 && out.leftStick.x == 192 && (out.buttons & SCE_PAD_BUTTON_PS));
    out = report(input, sizeof(input));
    assert(out.buttons & SCE_PAD_BUTTON_PS); /* Guide held across normal reports. */
    guide[4] = 0;
    out = report(guide, sizeof(guide));
    assert(out.analogButtons.r2 == 255 && out.leftStick.x == 192 && !(out.buttons & SCE_PAD_BUTTON_PS));
    /* Guide is a real command even when steering and pedals have not changed. */
    usleep(1550000);
    wheel_telemetry_t guide_telemetry;
    wheel_telemetry_get(&guide_telemetry); assert(!guide_telemetry.fresh);
    wheel_diagnostics_t guide_before, guide_after;
    wheel_diagnostics_get(&guide_before);
    guide[4] = 1; report(guide, sizeof(guide));
    wheel_telemetry_get(&guide_telemetry); wheel_diagnostics_get(&guide_after);
    assert(guide_telemetry.fresh && guide_telemetry.btn_xbox);
    assert(guide_after.input_packets == guide_before.input_packets + 1);
    guide[4] = 0; report(guide, sizeof(guide));
    input[4] = 0; input[5] = 0x20;
    report(input, sizeof(input)); report(input, sizeof(input));
    wheel_telemetry_t telemetry;
    wheel_telemetry_get(&telemetry);
    assert(telemetry.gear == 2); /* No repeated shifts while paddle stays held. */
    wheel_reset_state();
    wheel_telemetry_get(&telemetry);
    assert(!telemetry.connected && telemetry.gear == 1 && !telemetry.throttle_pct);
    report(input, sizeof(input));
    wheel_telemetry_get(&telemetry);
    assert(telemetry.gear == 2); /* New session resets the paddle edge. */

    wheel_config_t cfg, before;
    wheel_config_get(&cfg);
    const char *bad[] = {"-1", "2001", "4294967296", "99999999999999999999999", "1x", "", "+1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(wheel_config_set_value(&cfg, "deadzone", bad[i]) != 0);
    assert(wheel_config_set_value(&cfg, "unknown", "0") != 0);
    assert(wheel_config_set_value(&cfg, "btn_a", "3") != 0);
    assert(wheel_config_set_value(&cfg, "btn_a", "2147483648") != 0);
    assert(wheel_config_set_value(&cfg, "invert_pedals", "2") != 0);
    assert(wheel_config_set_value(&cfg, "deadzone", "0") == 0);
    assert(wheel_config_set_value(&cfg, "btn_paddle_r", "512") == 0);
    assert(wheel_config_apply(&cfg) == 0);
    input[8] = input[9] = 0;
    assert(report(input, sizeof(input)).analogButtons.r2 == 255);
    input[5] = 0;
    assert(report(input, sizeof(input)).analogButtons.r2 == 0);
    assert(wheel_config_set_value(&cfg, "invert_pedals", "1") == 0);
    assert(wheel_config_apply(&cfg) == 0);
    input[8] = 0xe8; input[9] = 3;
    out = report(input, sizeof(input));
    assert(out.analogButtons.l2 == 255 && out.analogButtons.r2 == 0);
    wheel_config_load();
    wheel_config_get(&cfg);
    assert(cfg.invert_pedals == 1 && cfg.deadzone == 0);
    before = cfg;
    assert(mkdir(WHEEL_CFG_DIR "/wheel_config.txt.tmp", 0700) == 0);
    cfg.deadzone = 1000;
    assert(wheel_config_apply(&cfg) != 0);
    wheel_config_get(&cfg);
    assert(cfg.deadzone == before.deadzone);
    assert(rmdir(WHEEL_CFG_DIR "/wheel_config.txt.tmp") == 0);
    wheel_config_load();
    wheel_config_get(&cfg);
    assert(cfg.deadzone == before.deadzone); /* Previous persisted file survives. */

    /* Invalid values in an existing file are ignored instead of underflowing. */
    FILE *f = fopen(WHEEL_CFG_DIR "/wheel_config.txt", "w");
    assert(f);
    fputs("deadzone=4294967295\nbtn_a=3\ninvert_pedals=2\nbtn_menu=1\n", f);
    fclose(f);
    wheel_config_load();
    wheel_config_get(&cfg);
    assert(cfg.deadzone == 368 && cfg.btn_a == SCE_PAD_BUTTON_CROSS && cfg.invert_pedals == 0 && cfg.btn_menu == 1);
    input[8] = input[9] = 0;
    input[7] = 128;
    pthread_t tid;
    assert(pthread_create(&tid, NULL, reader, NULL) == 0);
    for (int i = 0; i < 5000; i++) {
        wheel_telemetry_get(&telemetry);
        assert(telemetry.throttle_pct <= 100 && telemetry.brake_pct <= 100);
        wheel_config_get(&cfg);
        assert(cfg.deadzone == 368);
    }
    pthread_join(tid, NULL);
    send_result = -1;
    send_count = 0;
    assert(wheel_gip_handshake(-1, eps) != 0 && send_count == 1);
    uint8_t ack_input[12]; memcpy(ack_input, input, sizeof(input)); ack_input[1] = 0x10;
    assert(wheel_handle_packet(-1, eps, ack_input, sizeof(ack_input), &out) < 0);
    send_result = 0;
    send_count = 0;
    assert(wheel_gip_handshake(-1, eps) == 0 && send_count == 3);
    send_result = -110;
    assert(wheel_gip_handshake(-1, eps) == -110);
    assert(wheel_handle_packet(-1, eps, ack_input, sizeof(ack_input), &out) == -110);
    send_result = 0;

    /* Calibrated asymmetric steering and independent pedal spans. */
    wheel_config_default(&cfg);
    cfg.steer_min = 1000; cfg.steer_center = 30000; cfg.steer_max = 60000;
    cfg.deadzone = 0; cfg.throttle_min = 100; cfg.throttle_max = 900;
    cfg.brake_min = 50; cfg.brake_max = 850;
    assert(wheel_config_apply(&cfg) == 0);
    memset(input + 4, 0, 8);
    input[6] = 0x30; input[7] = 0x75; /* center 30000 */
    input[8] = 100; input[10] = 50;
    out = report(input, sizeof(input));
    assert(out.leftStick.x == 128 && out.analogButtons.r2 == 0 && out.analogButtons.l2 == 0);
    input[6] = 0xe8; input[7] = 3;
    assert(report(input, sizeof(input)).leftStick.x == 0);
    input[6] = 0x60; input[7] = 0xea;
    assert(report(input, sizeof(input)).leftStick.x == 255);
    input[6] = 0xc8; input[7] = 0xaf; /* 45000: right halfway */
    input[8] = 0xf4; input[9] = 1; input[10] = 0xc2; input[11] = 1; /* 500 / 450 */
    out = report(input, sizeof(input));
    assert(out.leftStick.x == 192 && out.analogButtons.r2 == 127 && out.analogButtons.l2 == 127);
    cfg.curve = 1; assert(wheel_config_apply(&cfg) == 0);
    assert(report(input, sizeof(input)).leftStick.x == 160);
    cfg.curve = 2; assert(wheel_config_apply(&cfg) == 0);
    assert(report(input, sizeof(input)).leftStick.x == 223);
    cfg.curve = 0; cfg.sensitivity = 200; assert(wheel_config_apply(&cfg) == 0);
    assert(report(input, sizeof(input)).leftStick.x == 255);
    cfg.sensitivity = 100; cfg.smoothing_ms = 50; assert(wheel_config_apply(&cfg) == 0);
    wheel_reset_state(); input[6] = 0x30; input[7] = 0x75;
    report(input, sizeof(input)); usleep(1000);
    input[6] = 0x60; input[7] = 0xea;
    out = report(input, sizeof(input)); assert(out.leftStick.x >= 128 && out.leftStick.x < 255);
    wheel_reset_state(); assert(report(input, sizeof(input)).leftStick.x == 255);
    wheel_telemetry_get(&telemetry);
    assert(telemetry.raw_steer == 60000 && telemetry.raw_throttle == 500 && telemetry.raw_brake == 450 && telemetry.fresh);
    wheel_diagnostics_t fresh_diag; wheel_diagnostics_get(&fresh_diag);
    assert(fresh_diag.age_ms < 1500);
    assert(wheel_set_gear(-1) == 0 && wheel_set_gear(9) != 0);
    wheel_telemetry_get(&telemetry); assert(telemetry.gear == -1);
    cfg.steer_min = cfg.steer_center; assert(wheel_config_apply(&cfg) != 0);
    cfg.steer_min = 1000; cfg.throttle_max = cfg.throttle_min; assert(wheel_config_apply(&cfg) != 0);
    cfg.throttle_max = 900;
    wheel_profile_t profiles[WHEEL_PROFILE_COUNT], loaded_profiles[WHEEL_PROFILE_COUNT];
    wheel_config_default(&before); assert(wheel_config_apply(&before) == 0);
    for (unsigned i = 0; i < WHEEL_PROFILE_COUNT; i++) {
        char name[WHEEL_PROFILE_NAME]; snprintf(name, sizeof(name), "Jogo %u ação \\\"", i + 1);
        cfg.sensitivity = 100 + i * 5;
        assert(wheel_profile_save(i, name, &cfg) == 0);
    }
    wheel_config_get(&before); assert(before.sensitivity == 100 && before.steer_min == 0); /* saving draft never activates it */
    wheel_profiles_get(profiles); wheel_config_load(); wheel_profiles_get(loaded_profiles);
    assert(memcmp(profiles, loaded_profiles, sizeof(profiles)) == 0);
    assert(wheel_profile_load(7) == 0); wheel_config_get(&cfg); assert(cfg.sensitivity == 135 && cfg.steer_min == 1000);
    assert(wheel_profile_save(8, "invalid", &cfg) != 0);
    assert(wheel_profile_save(0, "\xc0\xaf", &cfg) != 0);
    assert(wheel_profile_save(0, " \t", &cfg) != 0);
    assert(mkdir(WHEEL_CFG_DIR "/wheel_config.txt.tmp", 0700) == 0);
    wheel_config_default(&before);
    assert(wheel_backup_apply(&before, profiles) != 0);
    wheel_config_get(&cfg); assert(cfg.sensitivity == 135);
    assert(rmdir(WHEEL_CFG_DIR "/wheel_config.txt.tmp") == 0);
    assert(wheel_config_apply(&before) == 0); wheel_profiles_get(loaded_profiles);
    assert(memcmp(profiles, loaded_profiles, sizeof(profiles)) == 0); /* factory reset keeps profiles */
    profiles[0].config.steer_center = 0;
    assert(wheel_backup_apply(&before, profiles) != 0);
    assert(wheel_profile_delete(7) == 0 && wheel_profile_load(7) != 0);
    wheel_config_load(); wheel_profiles_get(profiles); assert(profiles[7].name[0] == 0);
    wheel_diagnostics_event(WHEEL_USB_OPEN, 0); wheel_diagnostics_event(WHEEL_READY, 0);
    wheel_diagnostics_injection(-123); wheel_diagnostics_injection(0);
    wheel_diagnostics_event(WHEEL_ERROR, 17);
    wheel_diagnostics_t diag; wheel_diagnostics_get(&diag);
    assert(diag.usb_found && diag.handshake_ok && diag.virtual_ready && diag.injection_attempts == 2 && diag.injection_errors == 1 && diag.last_error == 17 && diag.input_packets > 0);
    wheel_diagnostics_event(WHEEL_DISCONNECTED, 0); wheel_reset_state();
    wheel_diagnostics_get(&diag); assert(!diag.usb_found && !diag.virtual_ready && diag.rate_hz == 0);
    wheel_telemetry_get(&telemetry); assert(!telemetry.fresh);

    /* Button instrumentation: edges, send outcome, GIP sequence gaps and USB polls. */
    wheel_config_default(&cfg); assert(wheel_config_apply(&cfg) == 0);
    wheel_reset_state(); wheel_buttons_reset();
    wheel_diagnostics_event(WHEEL_READY, 0);
    wheel_button_report_t *br = malloc(sizeof(*br));
    assert(br);
    uint8_t pkt[12] = {0x20, 0, 10, 8, 0, 0, 0, 128, 0, 0, 0, 0};
    report(pkt, sizeof(pkt));                    /* nothing held: no event */
    wheel_diagnostics_injection(0);
    pkt[2] = 11; pkt[4] = 0x10; report(pkt, sizeof(pkt)); wheel_diagnostics_injection(0);  /* A down */
    usleep(20000);
    pkt[2] = 12; pkt[4] = 0; report(pkt, sizeof(pkt)); wheel_diagnostics_injection(0);     /* A up */
    pkt[2] = 15; pkt[4] = 0x20; report(pkt, sizeof(pkt)); wheel_diagnostics_injection(-5); /* B down, 2 lost */
    pkt[2] = 15; pkt[4] = 0x60; report(pkt, sizeof(pkt)); wheel_diagnostics_not_sent();    /* X down, repeated seq */
    pkt[2] = 16; pkt[4] = 0x80; report(pkt, sizeof(pkt));                                  /* Y down, never injected */
    pkt[2] = 17; pkt[4] = 0; pkt[5] = 0x20; report(pkt, sizeof(pkt)); wheel_diagnostics_injection(0);
    uint8_t guide_down[5] = {0x07, 0, 1, 1, 1};
    report(guide_down, sizeof(guide_down)); wheel_diagnostics_injection(0);
    uint8_t status_pkt[8] = {0x03, 0, 1, 4, 0x80, 0, 0, 0};
    assert(wheel_handle_packet(-1, eps, status_pkt, sizeof(status_pkt), &out) == 0);
    assert(wheel_handle_packet(-1, eps, pkt, 8, &out) == 0);
    wheel_diagnostics_usb_poll(WHEEL_POLL_TIMEOUT);
    wheel_diagnostics_usb_poll(WHEEL_POLL_DISCARDED);
    wheel_diagnostics_usb_poll(WHEEL_POLL_DATA);
    wheel_telemetry_get(&telemetry);
    assert(telemetry.link_age_ms < 1500 && telemetry.press_count[WHEEL_BTN_A] == 1);
    assert(telemetry.last_button == WHEEL_BTN_XBOX && telemetry.last_outcome == WHEEL_SEND_OK && telemetry.last_button_age_ms < 1500);
    wheel_buttons_get(br);
    const wheel_button_stats_t *bs = br->buttons;
    assert(bs[WHEEL_BTN_A].presses == 1 && bs[WHEEL_BTN_A].releases == 1 && bs[WHEEL_BTN_A].sent_ok == 1);
    assert(bs[WHEEL_BTN_A].min_hold_ms >= 20 && bs[WHEEL_BTN_A].last_hold_ms == bs[WHEEL_BTN_A].min_hold_ms);
    assert(bs[WHEEL_BTN_B].presses == 1 && bs[WHEEL_BTN_B].send_failed == 1 && bs[WHEEL_BTN_B].releases == 1);
    assert(bs[WHEEL_BTN_X].presses == 1 && bs[WHEEL_BTN_X].not_sent == 1 && bs[WHEEL_BTN_X].sent_ok == 0);
    assert(bs[WHEEL_BTN_Y].presses == 1 && bs[WHEEL_BTN_Y].not_sent == 1 && bs[WHEEL_BTN_Y].releases == 1);
    assert(bs[WHEEL_BTN_PADDLE_R].sent_ok == 1 && bs[WHEEL_BTN_XBOX].sent_ok == 1);
    assert(br->seq_lost == 2 && br->seq_repeated == 1 && br->sessions == 1);
    assert(br->other_packets == 1 && br->last_other_cmd == 0x03 && br->last_other_len == 8 && br->short_packets == 1);
    assert(br->usb_polls == 3 && br->usb_timeouts == 1 && br->usb_discarded == 1);
    assert(br->event_total == 7 && br->event_count == 7);
    assert(br->events[0].pressed == 1u << WHEEL_BTN_A && br->events[0].seq == 11 && br->events[0].outcome == WHEEL_SEND_OK);
    assert(br->events[1].released == 1u << WHEEL_BTN_A && br->events[1].hold_ms >= 20);
    assert(br->events[2].seq_lost == 2 && br->events[2].outcome == WHEEL_SEND_FAILED && br->events[2].inject_result == -5);
    assert(br->events[2].pad_buttons & SCE_PAD_BUTTON_CIRCLE);
    assert(br->events[4].outcome == WHEEL_SEND_SKIPPED && br->events[6].cmd == 0x07);
    assert(br->events[6].held == ((1u << WHEEL_BTN_PADDLE_R) | (1u << WHEEL_BTN_XBOX)));
    /* A reconnect keeps the counters but starts a new sequence and link. */
    wheel_reset_state();
    wheel_telemetry_get(&telemetry);
    assert(telemetry.link_age_ms == UINT32_MAX && telemetry.press_count[WHEEL_BTN_A] == 1);
    memset(pkt + 4, 0, 8);
    const uint8_t wrap[] = {254, 255, 1, 2};
    for (size_t i = 0; i < sizeof(wrap); i++) { pkt[2] = wrap[i]; report(pkt, sizeof(pkt)); }
    wheel_buttons_get(br); assert(br->seq_lost == 2);
    pkt[2] = 4; report(pkt, sizeof(pkt));
    wheel_buttons_get(br); assert(br->seq_lost == 3);
    /* The ring keeps the newest events in order. */
    for (int i = 0; i < 200; i++) {
        pkt[2] = (uint8_t)(5 + i % 200); pkt[4] = i % 2 ? 0 : 0x10;
        report(pkt, sizeof(pkt)); wheel_diagnostics_injection(0);
    }
    wheel_buttons_get(br);
    assert(br->event_total == 207 && br->event_count == WHEEL_EVENT_COUNT);
    assert(br->events[0].id == 80 && br->events[WHEEL_EVENT_COUNT - 1].id == 207);
    assert(br->buttons[WHEEL_BTN_A].presses == 101 && br->buttons[WHEEL_BTN_A].sent_ok == 101 && br->seq_lost == 3);
    wheel_buttons_reset();
    wheel_buttons_get(br); wheel_telemetry_get(&telemetry);
    assert(!br->event_total && !br->event_count && !br->buttons[WHEEL_BTN_A].presses && !br->seq_lost);
    assert(telemetry.last_button == -1 && !telemetry.press_count[WHEEL_BTN_A]);
    for (unsigned i = 0; i < WHEEL_BTN_COUNT; i++) assert(wheel_button_name(i));
    assert(!wheel_button_name(WHEEL_BTN_COUNT));
    free(br);
    puts("PASS: button edges, send outcomes, hold times, GIP sequence gaps and wrap, short/other packets, USB polls, event ring and counter reset");
    puts("PASS: calibrated steering/pedals, curves, sensitivity, smoothing reset, raw freshness, eight persistent profiles, atomic backup failures, defaults and diagnostics");

    /* PS Move shifter: extra buttons ride on the wheel's report and can be resent alone. */
    wheel_reset_state();
    ScePadData shifted;
    assert(!wheel_compose_pad(SCE_PAD_BUTTON_R1, &shifted));       /* no wheel report yet */
    uint8_t move_pkt[12] = {0x20, 0, 1, 8, 0, 0, 0, 128, 0, 0, 0, 0};
    ScePadData from_wheel;
    wheel_parse_input(move_pkt, &from_wheel);
    assert(from_wheel.buttons & SCE_PAD_BUTTON_R1);                   /* still held from above */
    assert(wheel_compose_pad(SCE_PAD_BUTTON_L1, &shifted));
    assert((shifted.buttons & SCE_PAD_BUTTON_L1) && !(shifted.buttons & SCE_PAD_BUTTON_R1));
    assert(shifted.leftStick.x == from_wheel.leftStick.x);
    assert(wheel_compose_pad(SCE_PAD_BUTTON_R2, &shifted) && shifted.analogButtons.r2 == 255);
    assert(wheel_compose_pad(0, &shifted) && !(shifted.buttons & (SCE_PAD_BUTTON_L1 | SCE_PAD_BUTTON_R2)));
    assert(shifted.analogButtons.r2 == from_wheel.analogButtons.r2);
    wheel_parse_input(move_pkt, &from_wheel);
    assert(!(from_wheel.buttons & SCE_PAD_BUTTON_R1));
    wheel_telemetry_get(&telemetry);
    int gear = telemetry.gear;
    wheel_virtual_shift(1); wheel_virtual_shift(1); wheel_virtual_shift(0);
    wheel_telemetry_get(&telemetry);
    assert(telemetry.gear == gear + 1);
    /* The Guide packet must not bring back a shift button that was already released. */
    for (int i = 0; i < 6; i++) { move_pkt[2] = (uint8_t)(10 + i); assert(wheel_handle_packet(-1, eps, move_pkt, sizeof(move_pkt), &from_wheel) == 1); }
    assert(wheel_compose_pad(SCE_PAD_BUTTON_R1, &shifted) && (shifted.buttons & SCE_PAD_BUTTON_R1));
    move_pkt[2] = 16; wheel_handle_packet(-1, eps, move_pkt, sizeof(move_pkt), &from_wheel);
    assert(wheel_compose_pad(0, &shifted));
    uint8_t guide_pkt[5] = {0x07, 0, 1, 1, 1};
    ScePadData guided; memset(&guided, 0, sizeof(guided));
    assert(wheel_handle_packet(-1, eps, guide_pkt, sizeof(guide_pkt), &guided) == 1);
    assert(!(guided.buttons & SCE_PAD_BUTTON_R1) && (guided.buttons & SCE_PAD_BUTTON_PS));
    puts("PASS: PS Move shifter extra buttons, resend without the wheel, gear count, Guide after a shift");

    puts("PASS: packets, Guide state, pedals, shifting, config persistence, snapshots, handshake failures");
    return 0;
}
