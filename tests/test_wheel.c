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
    if (argc == 3 && !strcmp(argv[1], "--serve")) {
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
    puts("PASS: calibrated steering/pedals, curves, sensitivity, smoothing reset, raw freshness, eight persistent profiles, atomic backup failures, defaults and diagnostics");

    puts("PASS: packets, Guide state, pedals, shifting, config persistence, snapshots, handshake failures");
    return 0;
}
