/* web_server.c — Servidor Web de Configuração e Overlay de Telemetria do Volante Ferrari 458 no PS5
 * Porta: 8088
 */

#include "web_server.h"
#include "controller_wheel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/time.h>
#include <errno.h>
#include <stdarg.h>
#include <strings.h>

#ifdef __PROSPERO__
#include <ps5/klog.h>
#define LOG(...) klog_printf("[HTTP] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#include "web_ui.h"

static int send_all(int fd, const char *data, size_t len) {
    size_t total = 0;
    while (total < len) {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, data + total, len - total, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, data + total, len - total, 0);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static void respond(int fd, const char *status, const char *type, const char *body) {
    char header[256];
    size_t len = strlen(body);
    snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", status, type, len);
    if (send_all(fd, header, strlen(header)) == 0) send_all(fd, body, len);
}

typedef struct { char *data; size_t capacity, used; int failed; } json_buffer_t;
static void append_json(json_buffer_t *b, const char *fmt, ...) {
    if (b->failed) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b->data + b->used, b->capacity - b->used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->capacity - b->used) b->failed = 1;
    else b->used += (size_t)n;
}
static void json_name(json_buffer_t *b, const char *name) {
    append_json(b, "\"");
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p == '"' || *p == '\\') append_json(b, "\\%c", *p);
        else if (*p < 32) append_json(b, "\\u%04x", *p);
        else append_json(b, "%c", *p);
    }
    append_json(b, "\"");
}
static void config_json(json_buffer_t *b, const wheel_config_t *cfg) {
    append_json(b, "{");
    for (size_t i = 0; i < wheel_config_field_count(); i++)
        append_json(b, "%s\"%s\":%u", i ? "," : "", wheel_config_field_name(i), wheel_config_field_value(cfg, i));
    append_json(b, "}");
}
static void profiles_json(json_buffer_t *b, const wheel_profile_t profiles[WHEEL_PROFILE_COUNT]) {
    append_json(b, "[");
    for (unsigned i = 0; i < WHEEL_PROFILE_COUNT; i++) {
        append_json(b, "%s", i ? "," : "");
        if (!profiles[i].name[0]) { append_json(b, "null"); continue; }
        append_json(b, "{\"slot\":%u,\"name\":", i + 1);
        json_name(b, profiles[i].name);
        append_json(b, ",\"config\":");
        config_json(b, &profiles[i].config);
        append_json(b, "}");
    }
    append_json(b, "]");
}
static void respond_json(int fd, const json_buffer_t *b) {
    if (b->failed) respond(fd, "500 Internal Server Error", "application/json", "{\"error\":\"Resposta muito longa\"}");
    else respond(fd, "200 OK", "application/json; charset=utf-8", b->data);
}
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int decode_form(char *s) {
    char *out = s;
    while (*s) {
        if (*s == '%') {
            if (!s[1] || !s[2]) return -1;
            int a = hex_digit(s[1]), b = hex_digit(s[2]);
            if (a < 0 || b < 0 || (a == 0 && b == 0)) return -1;
            *out++ = (char)(a * 16 + b); s += 3;
        } else { *out++ = *s == '+' ? ' ' : *s; s++; }
    }
    *out = '\0';
    return 0;
}
typedef struct { char *key, *value; } form_pair_t;
#define FORM_MAX 256
static int parse_form(char *body, form_pair_t pairs[FORM_MAX]) {
    if (!body || !*body) return -1;
    int count = 0;
    char *saveptr = NULL;
    for (char *token = strtok_r(body, "&", &saveptr); token; token = strtok_r(NULL, "&", &saveptr)) {
        if (count == FORM_MAX) return -1;
        char *eq = strchr(token, '=');
        if (!eq) return -1;
        *eq++ = '\0';
        if (decode_form(token) != 0 || decode_form(eq) != 0 || !*token) return -1;
        for (int i = 0; i < count; i++) if (!strcmp(pairs[i].key, token)) return -1;
        pairs[count++] = (form_pair_t){ token, eq };
    }
    return count ? count : -1;
}
static const char *form_value(const form_pair_t *pairs, int n, const char *key) {
    for (int i = 0; i < n; i++) if (!strcmp(pairs[i].key, key)) return pairs[i].value;
    return NULL;
}
static int config_form(wheel_config_t *cfg, const form_pair_t *pairs, int n,
                       const char *prefix, uint32_t *mask) {
    size_t prefix_len = strlen(prefix);
    *mask = 0;
    for (int i = 0; i < n; i++) {
        if (strncmp(pairs[i].key, prefix, prefix_len)) continue;
        const char *key = pairs[i].key + prefix_len;
        if (!*prefix && (!strcmp(key, "slot") || !strcmp(key, "name"))) continue;
        if (wheel_config_set_value(cfg, key, pairs[i].value) != 0) return -1;
        for (size_t j = 0; j < wheel_config_field_count(); j++)
            if (!strcmp(key, wheel_config_field_name(j))) *mask |= 1u << j;
    }
    return wheel_config_validate(cfg);
}
static int profile_slot(const char *s) {
    if (!s || strlen(s) != 1 || *s < '1' || *s > '0' + WHEEL_PROFILE_COUNT) return -1;
    return *s - '1';
}
static int import_backup(const form_pair_t *pairs, int n) {
    const char *version = form_value(pairs, n, "version");
    if (!version || strcmp(version, "1")) return -2;
    wheel_config_t cfg;
    wheel_config_default(&cfg);
    wheel_profile_t profiles[WHEEL_PROFILE_COUNT] = {0};
    uint32_t mask, full_mask = (1u << wheel_config_field_count()) - 1u;
    if (config_form(&cfg, pairs, n, "cfg.", &mask) != 0 || mask != full_mask) return -2;
    unsigned expected = 1 + (unsigned)wheel_config_field_count();
    for (unsigned i = 0; i < WHEEL_PROFILE_COUNT; i++) {
        char prefix[16], name_key[24];
        snprintf(prefix, sizeof(prefix), "p%u.", i + 1);
        snprintf(name_key, sizeof(name_key), "%sname", prefix);
        const char *name = form_value(pairs, n, name_key);
        if (!name || strlen(name) >= WHEEL_PROFILE_NAME) return -2;
        expected++;
        snprintf(profiles[i].name, sizeof(profiles[i].name), "%s", name);
        wheel_config_default(&profiles[i].config);
        /* name is metadata: config_form handles only the numerical fields. */
        uint32_t found = 0;
        for (int j = 0; j < n; j++) {
            if (strncmp(pairs[j].key, prefix, strlen(prefix)) || !strcmp(pairs[j].key, name_key)) continue;
            const char *key = pairs[j].key + strlen(prefix);
            if (wheel_config_set_value(&profiles[i].config, key, pairs[j].value) != 0) return -2;
            for (size_t k = 0; k < wheel_config_field_count(); k++)
                if (!strcmp(key, wheel_config_field_name(k))) found |= 1u << k;
        }
        if (*name) {
            if (found != full_mask || wheel_config_validate(&profiles[i].config) != 0) return -2;
            expected += (unsigned)wheel_config_field_count();
        } else if (found) return -2;
    }
    if ((unsigned)n != expected) return -2;
    /* The core validates names as well; no file is touched on invalid input. */
    for (unsigned i = 0; i < WHEEL_PROFILE_COUNT; i++) {
        for (const unsigned char *p = (const unsigned char *)profiles[i].name; *p; p++)
            if (*p < 32 || *p == 127) return -2;
    }
    for (unsigned i = 0; i < WHEEL_PROFILE_COUNT; i++)
        if (profiles[i].name[0] && !wheel_profile_name_valid(profiles[i].name)) return -2;
    return wheel_backup_apply(&cfg, profiles);
}

static void handle_client(int cfd) {
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    enum { HEADER_LIMIT = 4096, BODY_LIMIT = 16384 };
    char *req = malloc(HEADER_LIMIT + BODY_LIMIT + 1);
    if (!req) { respond(cfd, "503 Service Unavailable", "application/json", "{\"error\":\"Memoria indisponivel\"}"); close(cfd); return; }
    size_t used = 0;
    struct timeval began, now;
    gettimeofday(&began, NULL);
    char *header_end = NULL;
    while (used < HEADER_LIMIT - 1) {
        ssize_t got = recv(cfd, req + used, HEADER_LIMIT - 1 - used, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) goto done;
        used += (size_t)got;
        req[used] = '\0';
        if ((header_end = strstr(req, "\r\n\r\n"))) break;
        gettimeofday(&now, NULL);
        if ((now.tv_sec - began.tv_sec) * 1000000L + now.tv_usec - began.tv_usec >= 1000000L) goto done;
    }
    if (!header_end) {
        respond(cfd, "431 Request Header Fields Too Large", "application/json", "{\"error\":\"Requisicao muito longa\"}"); goto done;
    }
    size_t header_size = (size_t)(header_end + 4 - req), body_len = 0;
    int content_length_seen = 0, form_type = 0;
    char *line_end = strstr(req, "\r\n");
    if (!line_end || line_end == req) goto bad;
    *line_end = '\0';
    char method[8], target[2048], version[16], extra;
    if (sscanf(req, "%7s %2047s %15s %c", method, target, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) goto bad;
    for (char *line = line_end + 2; line < header_end; ) {
        char *end = strstr(line, "\r\n");
        if (!end) goto bad;
        *end = '\0';
        if (!strncasecmp(line, "Content-Length:", 15)) {
            if (content_length_seen++) goto bad;
            char *v = line + 15, *tail;
            while (*v == ' ' || *v == '\t') v++;
            if (*v < '0' || *v > '9') goto bad;
            errno = 0;
            unsigned long length = strtoul(v, &tail, 10);
            while (*tail == ' ' || *tail == '\t') tail++;
            if (errno || *tail || length > BODY_LIMIT) goto bad;
            body_len = (size_t)length;
        } else if (!strncasecmp(line, "Transfer-Encoding:", 18)) goto bad;
        else if (!strncasecmp(line, "Content-Type:", 13)) {
            char *v = line + 13;
            while (*v == ' ' || *v == '\t') v++;
            form_type = !strncasecmp(v, "application/x-www-form-urlencoded", 33) && (!v[33] || v[33] == ';');
        }
        line = end + 2;
    }
    int is_post = !strcmp(method, "POST");
    if (strcmp(method, "GET") && !is_post) {
        respond(cfd, "405 Method Not Allowed", "application/json", "{\"error\":\"Metodo nao suportado\"}"); goto done;
    }
    if (is_post && (!content_length_seen || !form_type)) goto bad;
    while (used < header_size + body_len) {
        ssize_t got = recv(cfd, req + used, header_size + body_len - used, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) goto bad;
        used += (size_t)got;
        gettimeofday(&now, NULL);
        if ((now.tv_sec - began.tv_sec) * 1000000L + now.tv_usec - began.tv_usec >= 2000000L) goto bad;
    }
    char *body = req + header_size;
    if (memchr(body, 0, body_len)) goto bad;
    body[body_len] = '\0';
    char *query = strchr(target, '?');
    if (query) *query++ = '\0';
    int mutation = !strcmp(target, "/api/save") || !strcmp(target, "/api/reset") ||
        !strcmp(target, "/api/import") || !strcmp(target, "/api/gear/reset") ||
        !strcmp(target, "/api/profiles/save") || !strcmp(target, "/api/profiles/load") || !strcmp(target, "/api/profiles/delete");
    if (is_post && !mutation) goto method_bad;
    if (mutation && !is_post && strcmp(target, "/api/save")) goto method_bad;
    if (mutation) {
        form_pair_t pairs[FORM_MAX];
        int n = parse_form(is_post ? body : query, pairs);
        if (n < 0) goto bad;
        wheel_config_t cfg; wheel_config_get(&cfg);
        uint32_t mask;
        int result = -1;
        if (!strcmp(target, "/api/save")) {
            if (form_value(pairs, n, "slot") || form_value(pairs, n, "name") || config_form(&cfg, pairs, n, "", &mask) != 0) goto bad;
            result = wheel_config_apply(&cfg);
        } else if (!strcmp(target, "/api/reset")) {
            if (n != 1 || !form_value(pairs, n, "reset") || strcmp(form_value(pairs, n, "reset"), "1")) goto bad;
            wheel_config_default(&cfg); result = wheel_config_apply(&cfg);
        } else if (!strcmp(target, "/api/gear/reset")) {
            const char *value = form_value(pairs, n, "gear");
            if (n != 1 || !value || (strcmp(value, "-1") && (strlen(value) != 1 || *value < '0' || *value > '8'))) goto bad;
            result = wheel_set_gear(atoi(value));
        } else if (!strcmp(target, "/api/import")) {
            result = import_backup(pairs, n);
            if (result == -2) goto bad;
        } else {
            int slot = profile_slot(form_value(pairs, n, "slot"));
            if (slot < 0) goto bad;
            if (!strcmp(target, "/api/profiles/save")) {
                const char *name = form_value(pairs, n, "name");
                if (!name || !wheel_profile_name_valid(name) || config_form(&cfg, pairs, n, "", &mask) != 0) goto bad;
                for (const unsigned char *p = (const unsigned char *)name; *p; p++) if (*p < 32 || *p == 127) goto bad;
                result = wheel_profile_save((unsigned)slot, name, &cfg);
            } else {
                if (n != 1) goto bad;
                wheel_profile_t profiles[WHEEL_PROFILE_COUNT]; wheel_profiles_get(profiles);
                if (!profiles[slot].name[0]) { respond(cfd, "404 Not Found", "application/json", "{\"error\":\"Perfil vazio\"}"); goto done; }
                result = !strcmp(target, "/api/profiles/load") ? wheel_profile_load((unsigned)slot) : wheel_profile_delete((unsigned)slot);
            }
        }
        if (result != 0) respond(cfd, "500 Internal Server Error", "application/json", "{\"error\":\"Nao foi possivel salvar no PS5\"}");
        else respond(cfd, "200 OK", "application/json", "{\"status\":\"ok\"}");
    } else if (!strcmp(target, "/api/telemetry")) {
        wheel_telemetry_t t; wheel_telemetry_get(&t);
        char json[768];
        snprintf(json, sizeof(json),
            "{\"connected\":%u,\"fresh\":%u,\"age_ms\":%u,\"raw_steer\":%u,\"raw_throttle\":%u,\"raw_brake\":%u,"
            "\"steer\":%d,\"throttle\":%u,\"brake\":%u,\"gear\":%d,"
            "\"paddle_l\":%u,\"paddle_r\":%u,\"btn_a\":%u,\"btn_b\":%u,"
            "\"btn_x\":%u,\"btn_y\":%u,\"btn_up\":%u,\"btn_down\":%u,"
            "\"btn_left\":%u,\"btn_right\":%u,\"btn_menu\":%u,\"btn_view\":%u,\"btn_xbox\":%u}",
            t.connected,t.fresh,t.age_ms,t.raw_steer,t.raw_throttle,t.raw_brake,
            t.steer_deg,t.throttle_pct,t.brake_pct,(int)t.gear,t.paddle_l,t.paddle_r,
            t.btn_a,t.btn_b,t.btn_x,t.btn_y,t.btn_up,t.btn_down,t.btn_left,t.btn_right,t.btn_menu,t.btn_view,t.btn_xbox);
        respond(cfd, "200 OK", "application/json", json);
    } else if (!strcmp(target, "/api/diagnostics")) {
        wheel_diagnostics_t d; wheel_diagnostics_get(&d);
        char json[768];
        snprintf(json, sizeof(json),
            "{\"stage\":%d,\"usb_found\":%d,\"handshake_ok\":%d,\"virtual_ready\":%d,\"age_ms\":%u,\"rate_hz\":%u,"
            "\"input_packets\":%llu,\"usb_errors\":%llu,\"injection_attempts\":%llu,\"injection_errors\":%llu,\"last_error\":%d,\"last_injection\":%d}",
            d.stage,d.usb_found,d.handshake_ok,d.virtual_ready,d.age_ms,d.rate_hz,
            (unsigned long long)d.input_packets,(unsigned long long)d.usb_errors,
            (unsigned long long)d.injection_attempts,(unsigned long long)d.injection_errors,d.last_error,d.last_injection);
        respond(cfd,"200 OK","application/json",json);
    } else if (!strcmp(target, "/api/config") || !strcmp(target, "/api/profiles") || !strcmp(target, "/api/backup")) {
        char *json = malloc(16384);
        if (!json) { respond(cfd,"503 Service Unavailable","application/json","{\"error\":\"Memoria indisponivel\"}"); goto done; }
        json_buffer_t buffer = { .data = json, .capacity = 16384 };
        wheel_config_t cfg; wheel_config_get(&cfg);
        if (!strcmp(target,"/api/config")) config_json(&buffer,&cfg);
        else {
            wheel_profile_t profiles[WHEEL_PROFILE_COUNT]; wheel_backup_get(&cfg, profiles);
            if (!strcmp(target,"/api/backup")) {
                append_json(&buffer,"{\"format\":\"ps5-wheel-backup\",\"version\":1,\"config\":");
                config_json(&buffer,&cfg); append_json(&buffer,",\"profiles\":");
            } else append_json(&buffer,"{\"profiles\":");
            profiles_json(&buffer,profiles); append_json(&buffer,"}");
        }
        respond_json(cfd,&buffer); free(json);
    } else if (!strcmp(target, "/overlay")) respond(cfd,"200 OK","text/html; charset=utf-8",HTML_OVERLAY);
    else if (!strcmp(target, "/")) respond(cfd,"200 OK","text/html; charset=utf-8",HTML_CONFIG);
    else respond(cfd,"404 Not Found","application/json","{\"error\":\"Pagina nao encontrada\"}");
    goto done;
method_bad:
    respond(cfd,"405 Method Not Allowed","application/json","{\"error\":\"Use POST para alterar ajustes\"}"); goto done;
bad:
    respond(cfd,"400 Bad Request","application/json","{\"error\":\"Confira os valores e a calibracao informados\"}");
done:
    free(req); close(cfd);
}


static void *web_server_thread(void *arg) {
    int sfd = (int)(intptr_t)arg;
    while (1) {
        struct sockaddr_in cin;
        socklen_t clen = sizeof(cin);
        int cfd = accept(sfd, (struct sockaddr *)&cin, &clen);
        if (cfd >= 0) handle_client(cfd);
        else if (errno != EINTR) {
            LOG("Erro no accept do web server: %d\n", errno);
            break;
        }
    }
    close(sfd);
    return NULL;
}

int web_server_start(int port) {
    if (port < 1 || port > 65535) { errno = EINVAL; return -1; }
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return -1;
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons((uint16_t)port);
    if (bind(sfd, (struct sockaddr *)&sin, sizeof(sin)) != 0 || listen(sfd, 16) != 0) {
        int error = errno; close(sfd); errno = error; return -1;
    }
    pthread_t tid;
    int error = pthread_create(&tid, NULL, web_server_thread, (void *)(intptr_t)sfd);
    if (error) { close(sfd); errno = error; return -1; }
    pthread_detach(tid);
    LOG("Servidor Web do Volante ouvindo na porta %d!\n", port);
    return 0;
}
