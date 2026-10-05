/* Legacy modules remain disabled by USB probing. Check their buffer bounds. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "controller_nintendo.h"
#include "controller_xbox.c" /* Expose static read_one to exercise USB cleanup. */

static struct usb_fs_endpoint *active;
static int mode, calls, closed, send_result;
int usleep(useconds_t us) { (void)us; return 0; }
int usb_send_out(int fd, struct usb_fs_endpoint *ep, const uint8_t *data,
                 uint32_t len, const char *tag) {
    (void)fd; (void)ep; (void)data; (void)len; (void)tag;
    return send_result;
}
int usb_send_cmd(int fd, struct usb_fs_endpoint *ep, uint8_t a, uint8_t b) {
    (void)fd; (void)ep; (void)a; (void)b;
    return send_result;
}
int ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    va_list ap; va_start(ap, request); void *arg = va_arg(ap, void *); va_end(ap);
    if (request == USB_FS_START) {
        if (mode == 0) { errno = EIO; return -1; }
        return 0;
    }
    if (request == USB_FS_STOP) return 0;
    if (request == USB_FS_CLOSE) { closed++; return 0; }
    assert(request == USB_FS_COMPLETE);
    calls++;
    if (mode == 3) { errno = EBUSY; return -1; }
    struct usb_fs_complete *complete = arg;
    complete->ep_index = calls == 1 ? 1 : 0;
    if (complete->ep_index == 0) {
        active[0].aFrames = 1;
        active[0].status = mode == 2 ? USB_ERR_TIMEOUT : 0;
        active[0].pLength[0] = mode == 4 ? 65 : 2;
        ((uint8_t *)active[0].ppBuffer[0])[0] = 2;
    }
    return 0;
}
int main(void) {
    struct usb_fs_endpoint eps[2] = {0}; active = eps;
    uint8_t buf[64], seq = 1;
    for (mode = 0; mode < 5; mode++) {
        calls = closed = 0;
        int ret = read_one(-1, eps, buf, 10);
        if (mode == 1) assert(ret == 2 && buf[0] == 2 && calls == 2);
        else if (mode == 2) assert(ret == 0);
        else assert(ret < 0);
        if (mode == 0 || mode == 3) assert(closed == 1);
        assert(eps[0].ppBuffer == NULL && eps[0].pLength == NULL);
    }
    int hs = HS_STREAMING;
    ScePadData pad = {0};
    uint8_t ack[12] = {0x21};
    assert(nintendo_handle_packet(-1, eps, ack, 12, &hs, &seq, &pad) == 0);
    uint8_t short_status[1] = {0x81};
    assert(nintendo_handle_packet(-1, eps, short_status, 1, &hs, &seq, &pad) == 0);
    assert(nintendo_handle_packet(-1, eps, NULL, 0, &hs, &seq, &pad) == 0);
    assert(xbox_handle_packet(-1, eps, NULL, 0, &pad) == 0);
    uint8_t short_guide[4] = {7};
    assert(xbox_handle_packet(-1, eps, short_guide, 4, &pad) == 0);
    assert(nintendo_send_subcmd(-1, eps, &seq, 0, buf, UINT32_MAX) < 0);
    assert(nintendo_send_subcmd(-1, eps, &seq, 0, NULL, 1) < 0);
    send_result = -1;
    hs = HS_WAIT_81_01;
    uint8_t status[2] = {0x81, 1};
    assert(nintendo_handle_packet(-1, eps, status, 2, &hs, &seq, &pad) < 0);
    assert(hs == HS_WAIT_81_01);
    puts("PASS: legacy packet bounds, length overflow, USB read cleanup and handshake failures");
}
