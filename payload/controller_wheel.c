/* controller_wheel.c — Thrustmaster Ferrari 458 Spider (044F:B671)
 * Driver de volante para Ghostcontrol no PS5 com suporte a configuracoes dinamicas.
 */

#include "controller_wheel.h"
#include "usb_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <limits.h>
#include <time.h>

#ifdef __PROSPERO__
#include <ps5/klog.h>
#define LOG(...) klog_printf("[WHEEL] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

static wheel_config_t g_wheel_cfg;
static wheel_profile_t g_profiles[WHEEL_PROFILE_COUNT];
static wheel_telemetry_t g_telemetry;
static wheel_diagnostics_t g_diagnostics;
static pthread_mutex_t g_wheel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_config_save_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_input_count;
static ScePadData g_last_pad;
static uint8_t g_guide_pressed, g_prev_pad_r, g_prev_pad_l;
static int8_t g_gear = 1;
static uint64_t g_last_input_us, g_last_steer_us, g_rate_start_us, g_rate_packets;
static float g_filtered_steer;
static int g_filter_valid;

static uint64_t now_us(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

void wheel_config_default(wheel_config_t *out) {
    *out = (wheel_config_t){
        .btn_xbox = SCE_PAD_BUTTON_PS, .btn_menu = SCE_PAD_BUTTON_OPTIONS,
        .btn_view = SCE_PAD_BUTTON_TOUCH_PAD, .btn_a = SCE_PAD_BUTTON_CROSS,
        .btn_b = SCE_PAD_BUTTON_CIRCLE, .btn_x = SCE_PAD_BUTTON_SQUARE,
        .btn_y = SCE_PAD_BUTTON_TRIANGLE, .btn_paddle_r = SCE_PAD_BUTTON_R1,
        .btn_paddle_l = SCE_PAD_BUTTON_L1, .deadzone = 368, .invert_pedals = 0,
        .steer_min = 0, .steer_center = 32768, .steer_max = 65535,
        .throttle_min = 8, .throttle_max = 1000, .brake_min = 8, .brake_max = 1000,
        .sensitivity = 100, .curve = 0, .smoothing_ms = 0
    };
}

typedef struct { const char *name; size_t offset; uint32_t maximum; int button; } config_field_t;
#define BUTTON(n) {#n, offsetof(wheel_config_t, n), 0, 1}
#define NUMBER(n, maximum) {#n, offsetof(wheel_config_t, n), maximum, 0}
static const config_field_t fields[] = {
    BUTTON(btn_xbox), BUTTON(btn_menu), BUTTON(btn_view), BUTTON(btn_a),
    BUTTON(btn_b), BUTTON(btn_x), BUTTON(btn_y), BUTTON(btn_paddle_r), BUTTON(btn_paddle_l),
    NUMBER(deadzone, 2000), NUMBER(invert_pedals, 1),
    NUMBER(steer_min, 65535), NUMBER(steer_center, 65535), NUMBER(steer_max, 65535),
    NUMBER(throttle_min, 1023), NUMBER(throttle_max, 1023),
    NUMBER(brake_min, 1023), NUMBER(brake_max, 1023),
    NUMBER(sensitivity, 200), NUMBER(curve, 2), NUMBER(smoothing_ms, 50)
};
#undef BUTTON
#undef NUMBER
size_t wheel_config_field_count(void) { return sizeof(fields) / sizeof(fields[0]); }
const char *wheel_config_field_name(size_t index) {
    return index < wheel_config_field_count() ? fields[index].name : NULL;
}
uint32_t wheel_config_field_value(const wheel_config_t *cfg, size_t index) {
    if (index >= wheel_config_field_count()) return 0;
    if (!strcmp(fields[index].name, "invert_pedals")) return (uint32_t)cfg->invert_pedals;
    uint32_t v;
    memcpy(&v, (const char *)cfg + fields[index].offset, sizeof(v));
    return v;
}
static int valid_button(uint32_t v) {
    const uint32_t mask = 0x0001ffffu | SCE_PAD_BUTTON_TOUCH_PAD;
    return !(v & ~mask) && (!v || !(v & (v - 1u)));
}
int wheel_config_set_value(wheel_config_t *cfg, const char *key, const char *value) {
    if (!cfg || !key || !value || *value < '0' || *value > '9') return -1;
    errno = 0;
    char *end;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno || *end || parsed > UINT32_MAX) return -1;
    uint32_t v = (uint32_t)parsed;
    for (size_t i = 0; i < wheel_config_field_count(); i++) {
        if (strcmp(key, fields[i].name)) continue;
        if (fields[i].button ? !valid_button(v) : v > fields[i].maximum) return -1;
        if (!strcmp(key, "invert_pedals")) cfg->invert_pedals = (int)v;
        else memcpy((char *)cfg + fields[i].offset, &v, sizeof(v));
        return 0;
    }
    return -1;
}
int wheel_config_validate(const wheel_config_t *cfg) {
    if (!cfg) return -1;
    for (size_t i = 0; i < wheel_config_field_count(); i++) {
        uint32_t v = wheel_config_field_value(cfg, i);
        if (fields[i].button ? !valid_button(v) : v > fields[i].maximum) return -1;
    }
    if (cfg->steer_min >= cfg->steer_center || cfg->steer_center >= cfg->steer_max ||
        cfg->steer_center - cfg->steer_min <= cfg->deadzone ||
        cfg->steer_max - cfg->steer_center <= cfg->deadzone ||
        cfg->throttle_min + 32 > cfg->throttle_max || cfg->brake_min + 32 > cfg->brake_max ||
        cfg->sensitivity < 50) return -1;
    return 0;
}
int wheel_profile_name_valid(const char *name) {
    if (!name) return 0;
    size_t n = strnlen(name, WHEEL_PROFILE_NAME);
    if (!n || n >= WHEEL_PROFILE_NAME) return 0;
    int visible = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127) return 0;
        if (c != ' ') visible = 1;
        /* Reject malformed UTF-8 so exported JSON always stays valid. */
        if (c >= 128) {
            int extra = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : -1;
            if (extra < 0 || i + (size_t)extra >= n) return 0;
            unsigned char next = (unsigned char)name[i+1];
            if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0) ||
                (c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)) return 0;
            for (int j = 1; j <= extra; j++) if (((unsigned char)name[i+j] & 0xc0) != 0x80) return 0;
            i += (size_t)extra;
        }
    }
    return visible;
}
#ifndef WHEEL_CFG_DIR
#define WHEEL_CFG_DIR "/data/ghostpad"
#endif
#define CFG_PATH WHEEL_CFG_DIR "/wheel_config.txt"
#define CFG_TEMP_PATH CFG_PATH ".tmp"
#define CFG_LEGACY_PATH "/data/ghostcontrol/wheel_config.txt"

void wheel_config_init_default(void) {
    pthread_mutex_lock(&g_wheel_lock);
    wheel_config_default(&g_wheel_cfg);
    g_filter_valid = 0;
    pthread_mutex_unlock(&g_wheel_lock);
}
void wheel_config_get(wheel_config_t *out) {
    pthread_mutex_lock(&g_wheel_lock);
    *out = g_wheel_cfg;
    pthread_mutex_unlock(&g_wheel_lock);
}
void wheel_profiles_get(wheel_profile_t out[WHEEL_PROFILE_COUNT]) {
    pthread_mutex_lock(&g_config_save_lock);
    memcpy(out, g_profiles, sizeof(g_profiles));
    pthread_mutex_unlock(&g_config_save_lock);
}
void wheel_backup_get(wheel_config_t *cfg, wheel_profile_t profiles[WHEEL_PROFILE_COUNT]) {
    pthread_mutex_lock(&g_config_save_lock);
    wheel_config_get(cfg);
    memcpy(profiles, g_profiles, sizeof(g_profiles));
    pthread_mutex_unlock(&g_config_save_lock);
}
static uint32_t input_age_locked(void) {
    uint64_t now = now_us();
    uint64_t age = g_last_input_us && now >= g_last_input_us ? (now - g_last_input_us) / 1000 : UINT32_MAX;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}
void wheel_telemetry_get(wheel_telemetry_t *out) {
    pthread_mutex_lock(&g_wheel_lock);
    *out = g_telemetry;
    out->age_ms = input_age_locked();
    out->fresh = out->connected && out->age_ms < 1500;
    pthread_mutex_unlock(&g_wheel_lock);
}
void wheel_diagnostics_get(wheel_diagnostics_t *out) {
    pthread_mutex_lock(&g_wheel_lock);
    *out = g_diagnostics;
    out->age_ms = input_age_locked();
    if (!g_telemetry.connected || out->age_ms >= 1500) { out->rate_hz = 0; out->virtual_ready = 0; }
    pthread_mutex_unlock(&g_wheel_lock);
}
void wheel_diagnostics_event(int stage, int32_t error) {
    pthread_mutex_lock(&g_wheel_lock);
    g_diagnostics.stage = stage;
    if (stage == WHEEL_USB_OPEN) g_diagnostics.usb_found = 1;
    if (stage == WHEEL_HANDSHAKE) g_diagnostics.handshake_ok = 0;
    if (stage == WHEEL_READY) g_diagnostics.handshake_ok = 1;
    if (stage == WHEEL_DISCONNECTED) {
        g_diagnostics.usb_found = g_diagnostics.handshake_ok = g_diagnostics.virtual_ready = 0;
    }
    if (stage == WHEEL_ERROR) { g_diagnostics.usb_errors++; g_diagnostics.last_error = error; }
    pthread_mutex_unlock(&g_wheel_lock);
}
void wheel_diagnostics_injection(int32_t result) {
    pthread_mutex_lock(&g_wheel_lock);
    g_diagnostics.injection_attempts++;
    g_diagnostics.last_injection = result;
    g_diagnostics.virtual_ready = result == 0;
    if (result != 0) g_diagnostics.injection_errors++;
    pthread_mutex_unlock(&g_wheel_lock);
}
int wheel_set_gear(int gear) {
    if (gear < -1 || gear > 8) return -1;
    pthread_mutex_lock(&g_wheel_lock);
    g_gear = (int8_t)gear;
    g_telemetry.gear = g_gear;
    pthread_mutex_unlock(&g_wheel_lock);
    return 0;
}
static int write_config(FILE *f, const wheel_config_t *cfg) {
    for (size_t i = 0; i < wheel_config_field_count(); i++)
        if (fprintf(f, "%s=%u\n", fields[i].name, wheel_config_field_value(cfg, i)) < 0) return -1;
    return 0;
}
/* One atomic file holds current settings and profiles. No partial imports. */
static int persist_locked(const wheel_config_t *cfg, const wheel_profile_t profiles[WHEEL_PROFILE_COUNT]) {
    if (mkdir(WHEEL_CFG_DIR, 0755) != 0 && errno != EEXIST) return -1;
    FILE *f = fopen(CFG_TEMP_PATH, "w");
    if (!f) return -1;
    int failed = write_config(f, cfg) != 0;
    for (unsigned i = 0; i < WHEEL_PROFILE_COUNT && !failed; i++) {
        if (!profiles[i].name[0]) continue;
        failed = fprintf(f, "[profile%u]\nname=%s\n", i + 1, profiles[i].name) < 0 ||
                 write_config(f, &profiles[i].config) != 0;
    }
    if (!failed && (fflush(f) != 0 || fsync(fileno(f)) != 0)) failed = 1;
    if (fclose(f) != 0) failed = 1;
    if (failed || rename(CFG_TEMP_PATH, CFG_PATH) != 0) { unlink(CFG_TEMP_PATH); return -1; }
    pthread_mutex_lock(&g_wheel_lock);
    if (memcmp(&g_wheel_cfg, cfg, sizeof(*cfg)) != 0) g_filter_valid = 0;
    g_wheel_cfg = *cfg;
    pthread_mutex_unlock(&g_wheel_lock);
    if (profiles != g_profiles) memcpy(g_profiles, profiles, sizeof(g_profiles));
    return 0;
}
int wheel_config_apply(const wheel_config_t *cfg) {
    if (wheel_config_validate(cfg) != 0) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&g_config_save_lock);
    int result = persist_locked(cfg, g_profiles);
    pthread_mutex_unlock(&g_config_save_lock);
    return result;
}
int wheel_backup_apply(const wheel_config_t *cfg, const wheel_profile_t profiles[WHEEL_PROFILE_COUNT]) {
    if (!profiles || wheel_config_validate(cfg) != 0) return -1;
    for (unsigned i = 0; i < WHEEL_PROFILE_COUNT; i++) {
        if (profiles[i].name[0] && (!wheel_profile_name_valid(profiles[i].name) || wheel_config_validate(&profiles[i].config) != 0)) return -1;
    }
    pthread_mutex_lock(&g_config_save_lock);
    int result = persist_locked(cfg, profiles);
    pthread_mutex_unlock(&g_config_save_lock);
    return result;
}
int wheel_profile_save(unsigned slot, const char *name, const wheel_config_t *cfg) {
    if (slot >= WHEEL_PROFILE_COUNT || !name || !wheel_profile_name_valid(name) || wheel_config_validate(cfg) != 0) return -1;
    pthread_mutex_lock(&g_config_save_lock);
    wheel_profile_t profiles[WHEEL_PROFILE_COUNT];
    memcpy(profiles, g_profiles, sizeof(profiles));
    snprintf(profiles[slot].name, sizeof(profiles[slot].name), "%s", name);
    profiles[slot].config = *cfg;
    wheel_config_t current;
    wheel_config_get(&current);
    int result = persist_locked(&current, profiles);
    pthread_mutex_unlock(&g_config_save_lock);
    return result;
}
int wheel_profile_load(unsigned slot) {
    if (slot >= WHEEL_PROFILE_COUNT) return -1;
    pthread_mutex_lock(&g_config_save_lock);
    int result = g_profiles[slot].name[0] ? persist_locked(&g_profiles[slot].config, g_profiles) : -1;
    pthread_mutex_unlock(&g_config_save_lock);
    return result;
}
int wheel_profile_delete(unsigned slot) {
    if (slot >= WHEEL_PROFILE_COUNT) return -1;
    pthread_mutex_lock(&g_config_save_lock);
    wheel_profile_t profiles[WHEEL_PROFILE_COUNT];
    memcpy(profiles, g_profiles, sizeof(profiles));
    memset(&profiles[slot], 0, sizeof(profiles[slot]));
    wheel_config_t current;
    wheel_config_get(&current);
    int result = persist_locked(&current, profiles);
    pthread_mutex_unlock(&g_config_save_lock);
    return result;
}
void wheel_config_load(void) {
    wheel_config_t cfg;
    wheel_config_default(&cfg);
    wheel_profile_t profiles[WHEEL_PROFILE_COUNT] = {0};
    for (unsigned i = 0; i < WHEEL_PROFILE_COUNT; i++) wheel_config_default(&profiles[i].config);
    FILE *f = fopen(CFG_PATH, "r");
    if (!f && errno == ENOENT) f = fopen(CFG_LEGACY_PATH, "r");
    if (f) {
        char line[256];
        int section = -1;
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '[') {
                unsigned slot; char extra;
                section = sscanf(line, "[profile%u]%c", &slot, &extra) == 1 && slot >= 1 && slot <= WHEEL_PROFILE_COUNT ? (int)slot - 1 : -2;
                continue;
            }
            if (section == -2) continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq++ = '\0';
            if (section >= 0 && !strcmp(line, "name")) {
                if (wheel_profile_name_valid(eq)) snprintf(profiles[section].name, sizeof(profiles[section].name), "%s", eq);
            } else {
                wheel_config_t *target = section < 0 ? &cfg : &profiles[section].config;
                if (wheel_config_set_value(target, line, eq) != 0) LOG("Configuracao invalida ignorada: %s\n", line);
            }
        }
        fclose(f);
    }
    if (wheel_config_validate(&cfg) != 0) { wheel_config_default(&cfg); LOG("Calibracao invalida; usando padrao\n"); }
    for (unsigned i = 0; i < WHEEL_PROFILE_COUNT; i++) {
        if (profiles[i].name[0] && wheel_config_validate(&profiles[i].config) != 0) memset(&profiles[i], 0, sizeof(profiles[i]));
    }
    pthread_mutex_lock(&g_config_save_lock);
    pthread_mutex_lock(&g_wheel_lock);
    g_wheel_cfg = cfg;
    g_filter_valid = 0;
    pthread_mutex_unlock(&g_wheel_lock);
    memcpy(g_profiles, profiles, sizeof(g_profiles));
    pthread_mutex_unlock(&g_config_save_lock);
}

void wheel_reset_state(void) {
    pthread_mutex_lock(&g_wheel_lock);
    g_input_count = 0;
    g_last_input_us = g_last_steer_us = g_rate_start_us = g_rate_packets = 0;
    g_filter_valid = 0;
    g_guide_pressed = g_prev_pad_r = g_prev_pad_l = 0;
    g_gear = 1;
    memset(&g_last_pad, 0, sizeof(g_last_pad));
    g_last_pad.leftStick.x = g_last_pad.leftStick.y = 128;
    g_last_pad.rightStick.x = g_last_pad.rightStick.y = 128;
    g_last_pad.quat.w = 1.0f;
    memset(&g_telemetry, 0, sizeof(g_telemetry));
    g_telemetry.gear = 1;
    pthread_mutex_unlock(&g_wheel_lock);
}

static uint8_t pedal_scale(uint16_t v, uint32_t minimum, uint32_t maximum) {
    if (v <= minimum) return 0;
    if (v >= maximum) return 255;
    return (uint8_t)(((uint32_t)(v - minimum) * 255u) / (maximum - minimum));
}
static float raw_steering(uint16_t raw, const wheel_config_t *cfg) {
    if (raw < cfg->steer_center) {
        if (raw <= cfg->steer_min) return -1.0f;
        return -(float)(cfg->steer_center - raw) / (cfg->steer_center - cfg->steer_min);
    }
    if (raw >= cfg->steer_max) return 1.0f;
    return (float)(raw - cfg->steer_center) / (cfg->steer_max - cfg->steer_center);
}
static uint8_t steering_output(uint16_t raw, uint64_t now) {
    const wheel_config_t *cfg = &g_wheel_cfg;
    /* Preserve the previous response exactly with factory calibration/settings. */
    if (cfg->steer_min == 0 && cfg->steer_center == 32768 && cfg->steer_max == 65535 &&
        cfg->sensitivity == 100 && cfg->curve == 0 && cfg->smoothing_ms == 0) {
        return raw >= 32768u - cfg->deadzone && raw <= 32768u + cfg->deadzone ? 128 : (uint8_t)(raw >> 8);
    }
    float x = 0;
    if (raw < cfg->steer_center && cfg->steer_center - raw > cfg->deadzone)
        x = -(float)(cfg->steer_center - raw - cfg->deadzone) / (cfg->steer_center - cfg->steer_min - cfg->deadzone);
    else if (raw > cfg->steer_center && raw - cfg->steer_center > cfg->deadzone)
        x = (float)(raw - cfg->steer_center - cfg->deadzone) / (cfg->steer_max - cfg->steer_center - cfg->deadzone);
    if (x < -1) x = -1;
    if (x > 1) x = 1;
    float magnitude = x < 0 ? -x : x;
    if (cfg->curve == 1) magnitude *= magnitude;
    else if (cfg->curve == 2) magnitude *= 2 - magnitude;
    x = (x < 0 ? -magnitude : magnitude) * cfg->sensitivity / 100.0f;
    if (x < -1) x = -1;
    if (x > 1) x = 1;
    if (cfg->smoothing_ms && g_filter_valid && g_last_steer_us && now > g_last_steer_us) {
        uint64_t dt = now - g_last_steer_us;
        if (dt < 500000) {
            float alpha = (float)dt / ((float)dt + cfg->smoothing_ms * 1000.0f);
            x = g_filtered_steer + alpha * (x - g_filtered_steer);
        }
    }
    g_filtered_steer = x;
    g_filter_valid = 1;
    return (uint8_t)(128 + x * (x < 0 ? 128 : 127) + 0.5f);
}

static void apply_guide_locked(ScePadData *pad) {
    if (!g_guide_pressed) return;
    pad->buttons |= g_wheel_cfg.btn_xbox;
    if (g_wheel_cfg.btn_xbox == SCE_PAD_BUTTON_R2) pad->analogButtons.r2 = 255;
    if (g_wheel_cfg.btn_xbox == SCE_PAD_BUTTON_L2) pad->analogButtons.l2 = 255;
}

static void record_input_locked(uint64_t now) {
    g_last_input_us = now;
    g_diagnostics.input_packets++;
    g_diagnostics.stage = WHEEL_STREAMING;
    if (!g_rate_start_us) { g_rate_start_us = now; g_rate_packets = 0; }
    g_rate_packets++;
    if (now > g_rate_start_us && now - g_rate_start_us >= 1000000u) {
        g_diagnostics.rate_hz = (uint32_t)(g_rate_packets * 1000000u / (now - g_rate_start_us));
        g_rate_start_us = now; g_rate_packets = 0;
    }
}

/* Parse do pacote GIP (cmd=0x20) do Ferrari 458 Spider */
static void parse_input_locked(const uint8_t *b, ScePadData *o) {
    uint8_t  b4     = b[4];
    uint8_t  b5     = b[5];
    uint16_t steer  = (uint16_t)b[6]  | ((uint16_t)b[7]  << 8);
    uint16_t acel16 = ((uint16_t)b[8]  | ((uint16_t)b[9]  << 8)) & 0x3FF;
    uint16_t freio16= ((uint16_t)b[10] | ((uint16_t)b[11] << 8)) & 0x3FF;

    uint64_t now = now_us();
    uint8_t acel = pedal_scale(acel16, g_wheel_cfg.throttle_min, g_wheel_cfg.throttle_max);
    uint8_t freio = pedal_scale(freio16, g_wheel_cfg.brake_min, g_wheel_cfg.brake_max);

    if (g_wheel_cfg.invert_pedals) {
        uint8_t tmp = acel;
        acel = freio;
        freio = tmp;
    }

    uint8_t stick_x = steering_output(steer, now);

    o->leftStick.x      = stick_x;
    o->leftStick.y      = 128u; /* neutro */
    o->rightStick.x     = 128u; /* neutro */
    o->rightStick.y     = 128u; /* neutro */
    o->analogButtons.r2 = acel;
    o->analogButtons.l2 = freio;

    uint32_t btn = 0;

    /* b[4]: Botoes principais configuraveis */
    if (b4 & 0x04u) btn |= g_wheel_cfg.btn_menu;
    if (b4 & 0x08u) btn |= g_wheel_cfg.btn_view;
    if (b4 & 0x10u) btn |= g_wheel_cfg.btn_a;
    if (b4 & 0x20u) btn |= g_wheel_cfg.btn_b;
    if (b4 & 0x40u) btn |= g_wheel_cfg.btn_x;
    if (b4 & 0x80u) btn |= g_wheel_cfg.btn_y;

    /* b[5]: Direcional e borboletas */
    if (b5 & 0x01u) btn |= SCE_PAD_BUTTON_UP;
    if (b5 & 0x02u) btn |= SCE_PAD_BUTTON_DOWN;
    if (b5 & 0x04u) btn |= SCE_PAD_BUTTON_LEFT;
    if (b5 & 0x08u) btn |= SCE_PAD_BUTTON_RIGHT;
    if (b5 & 0x10u) btn |= g_wheel_cfg.btn_paddle_l;
    if (b5 & 0x20u) btn |= g_wheel_cfg.btn_paddle_r;

    /* A button mapped to a trigger must also drive its analog value. */
    if (btn & SCE_PAD_BUTTON_R2) o->analogButtons.r2 = 255;
    if (btn & SCE_PAD_BUTTON_L2) o->analogButtons.l2 = 255;

    /* Gatilhos digitais com limiar */
    if (freio > 16u) btn |= SCE_PAD_BUTTON_L2;
    if (acel  > 16u) btn |= SCE_PAD_BUTTON_R2;

    /* Keep the normal report separate from the dedicated Guide packet. */
    o->buttons   = btn;
    o->connected = 1;
    o->quat.w    = 1.0f;
    g_last_pad = *o;
    apply_guide_locked(o);

    /* Controle de marchas inteligente com base nas borboletas */
    uint8_t cur_pad_r = (b5 & 0x20u) ? 1 : 0;
    uint8_t cur_pad_l = (b5 & 0x10u) ? 1 : 0;

    if (cur_pad_r && !g_prev_pad_r) {
        if (g_gear < 8) g_gear++;
    }
    if (cur_pad_l && !g_prev_pad_l) {
        if (g_gear > -1) g_gear--;
    }
    g_prev_pad_r = cur_pad_r;
    g_prev_pad_l = cur_pad_l;

    /* Atualiza telemetria em tempo real para o overlay */
    g_telemetry.steer_deg = (int16_t)(raw_steering(steer, &g_wheel_cfg) * 120);
    g_telemetry.raw_steer = steer;
    g_telemetry.raw_throttle = acel16;
    g_telemetry.raw_brake = freio16;
    g_last_steer_us = now;
    record_input_locked(now);
    g_telemetry.throttle_pct = (uint8_t)((acel * 100u) / 255u);
    g_telemetry.brake_pct = (uint8_t)((freio * 100u) / 255u);
    g_telemetry.paddle_l = cur_pad_l;
    g_telemetry.paddle_r = cur_pad_r;
    g_telemetry.gear = g_gear;
    g_telemetry.buttons = o->buttons;
    g_telemetry.connected = 1;

    g_telemetry.btn_menu  = (b4 & 0x04u) ? 1 : 0;
    g_telemetry.btn_view  = (b4 & 0x08u) ? 1 : 0;
    g_telemetry.btn_a     = (b4 & 0x10u) ? 1 : 0;
    g_telemetry.btn_b     = (b4 & 0x20u) ? 1 : 0;
    g_telemetry.btn_x     = (b4 & 0x40u) ? 1 : 0;
    g_telemetry.btn_y     = (b4 & 0x80u) ? 1 : 0;

    g_telemetry.btn_up    = (b5 & 0x01u) ? 1 : 0;
    g_telemetry.btn_down  = (b5 & 0x02u) ? 1 : 0;
    g_telemetry.btn_left  = (b5 & 0x04u) ? 1 : 0;
    g_telemetry.btn_right = (b5 & 0x08u) ? 1 : 0;
}

void wheel_parse_input(const uint8_t *b, ScePadData *o) {
    if (!b || !o) return;
    pthread_mutex_lock(&g_wheel_lock);
    parse_input_locked(b, o);
    pthread_mutex_unlock(&g_wheel_lock);
}

static int send_ack_wheel(int fd, struct usb_fs_endpoint *eps, const uint8_t *p, uint32_t len) {
    if (len < 4 || !(p[1] & 0x10)) return 0;
    uint8_t ack[13] = {0x01, 0x20, p[2], 0x09, 0x00, p[0], 0x20, p[3], 0x00, 0, 0, 0, 0};
    return usb_send_out(fd, &eps[1], ack, sizeof(ack), "wheel_ack");
}

/* Handshake do volante */
int wheel_gip_handshake(int fd, struct usb_fs_endpoint *eps) {
    static const uint8_t power_cfg[19] = {
        0x05, 0x20, 0x01, 0x0f, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x55, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    static const uint8_t power_on[5] = {0x05, 0x20, 0x02, 0x01, 0x00};
    static const uint8_t led_on[7]   = {0x0a, 0x20, 0x03, 0x03, 0x00, 0x01, 0x14};

    wheel_reset_state();
    wheel_diagnostics_event(WHEEL_HANDSHAKE, 0);

    int result = usb_send_out(fd, &eps[1], power_cfg, sizeof(power_cfg), "wheel_pcfg");
    if (result != 0) return result;
    usleep(15000);
    result = usb_send_out(fd, &eps[1], power_on, sizeof(power_on), "wheel_pon");
    if (result != 0) return result;
    usleep(15000);
    result = usb_send_out(fd, &eps[1], led_on, sizeof(led_on), "wheel_led");
    if (result != 0) return result;

    wheel_diagnostics_event(WHEEL_READY, 0);
    LOG("Thrustmaster 458 Spider handshake executado com sucesso!\n");
    return 0;
}

int wheel_handle_packet(int fd, struct usb_fs_endpoint *eps,
                        const uint8_t *buf, uint32_t len,
                        ScePadData *out_pad) {
    if (!buf || !out_pad || len < 4 || len > 64) return 0;
    uint8_t cmd = buf[0];

    int ack_result = send_ack_wheel(fd, eps, buf, len);
    if (ack_result != 0) return ack_result;

    if (cmd == 0x20 && len >= 12) {
        pthread_mutex_lock(&g_wheel_lock);
        if (g_input_count < 6) g_input_count++;
        parse_input_locked(buf, out_pad);
        pthread_mutex_unlock(&g_wheel_lock);
        return 1;
    }

    /* Pacote Guide / Xbox button (cmd=0x07) */
    if (cmd == 0x07 && len >= 5) {
        pthread_mutex_lock(&g_wheel_lock);
        g_guide_pressed = g_input_count > 5 && (buf[4] & 0x01u);
        *out_pad = g_last_pad;
        apply_guide_locked(out_pad);
        g_telemetry.btn_xbox = g_guide_pressed;
        g_telemetry.buttons = out_pad->buttons;
        int connected = g_last_pad.connected;
        if (connected) record_input_locked(now_us());
        pthread_mutex_unlock(&g_wheel_lock);
        if (!connected) return 0;
        return 1;
    }

    /* Pacote de anuncio ou status */
    if (cmd == 0x02 || cmd == 0x03) {
        return 0;
    }

    return 0;
}
