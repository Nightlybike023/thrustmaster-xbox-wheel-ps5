/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ghost-Control v5: Multi-controller support
 * USB HID controllers → virtual DualSense devices on PS5
 *
 * Supports up to MAX_SLOTS (4) simultaneous controllers:
 *   - 8BitDo / Nintendo Switch Pro  (VID=057E PID=2009)
 *   - Xbox One S                    (VID=045E PID=02EA)
 *
 * Each detected controller gets its own VDA device + force_bind
 * assignment dialog + VDI injection thread.
 * Hotplug: plug in any time, disconnect any time.
 */

#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usb_endian.h>
#include <dev/usb/usbdi.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#endif

#include "shellui_pad.h"
#include "gc_types.h"
#include "usb_helpers.h"
#include "controller_nintendo.h"
#include "controller_xbox.h"
#include "controller_wheel.h"
#include "web_server.h"
#include "instance_guard.h"
#include "controller_move.h"
#include "shifter.h"

/* ── Logging ──────────────────────────────────────────────────────────── */
#define LOG_DIR  "/data/ghostpad"
#define LOG_PATH "/data/ghostpad/gc_status.log"
#define PID_PATH "/data/ghostpad/gc_main.pid"
#define INSTANCE_PATH "/data/ghostpad/gc_instance.lock"
#define LOG_MAX  480

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_log_fd = -1;

void ghostpad_status_log_reset(void) {
    pthread_mutex_lock(&g_log_lock);
    if (g_log_fd >= 0) { close(g_log_fd); g_log_fd = -1; }
    mkdir(LOG_DIR, 0755);
    g_log_fd = open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    pthread_mutex_unlock(&g_log_lock);
}

void ghostpad_status_log(const char *fmt, ...) {
    char buf[LOG_MAX]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf)-1, fmt, ap); va_end(ap);
    buf[LOG_MAX-1] = '\0';
    klog_printf("%s", buf);
    pthread_mutex_lock(&g_log_lock);
    if (g_log_fd >= 0) {
        size_t n = strnlen(buf, sizeof(buf));
        write(g_log_fd, buf, n);
        if (n && buf[n-1] != '\n') write(g_log_fd, "\n", 1);
    }
    pthread_mutex_unlock(&g_log_lock);
}
#define gp_log(...) ghostpad_status_log("[GC] " __VA_ARGS__)

/* ── SCE stubs ────────────────────────────────────────────────────────── */
extern int32_t sceUserServiceInitialize(void *params);
extern int32_t sceUserServiceGetInitialUser(int32_t *outUserId);
extern int32_t sceUserServiceGetForegroundUser(int32_t *outUserId);
extern int32_t scePadInit(void);
extern int32_t scePadSetProcessPrivilege(int32_t privilege);
extern int32_t scePadGetHandle(int32_t userId, int32_t type, int32_t index);
extern int32_t scePadVirtualDeviceAddDevice(void *param, int32_t deviceType);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *padData);
extern int32_t sceKernelSendNotificationRequest(int unk0, void *req, size_t size, int unk1);

#define VIRTUAL_DEVICE_TYPE_DUALSENSE 3

/* ── Multi-controller slots ───────────────────────────────────────────── */
#define MAX_SLOTS 1

typedef struct {
    _Atomic int32_t handle;        /* VDA handle, -1 = slot free */
    _Atomic int     vdi_ready;
    _Atomic int     usb_active;    /* USB reader thread is running */
    _Atomic int     confirmed;     /* user pressed a button — assignment done */
    char             dev_path[32];  /* ugen path claimed by this slot */
    uint16_t         vid, pid;
    _Atomic uint32_t inject_count;
} ctrl_slot_t;

static ctrl_slot_t     g_slots[MAX_SLOTS];
static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t         g_inject_uid = 0x10000000;

/* Assignment serialization: only ONE controller may show its assignment
 * dialog at a time. g_assign_slot = slot awaiting user confirmation, or -1.
 * Without this, multiple dialogs stack and all bind to the same user. */
static _Atomic int     g_assign_slot = -1;

/* ── klog device-ID queue ─────────────────────────────────────────────── */
#define KLOG_QSIZE 16
static uint64_t        g_klog_q[KLOG_QSIZE];
static int             g_klog_qw = 0, g_klog_qr = 0;
static pthread_mutex_t g_klog_lock = PTHREAD_MUTEX_INITIALIZER;

static void klog_enqueue(uint64_t id) {
    pthread_mutex_lock(&g_klog_lock);
    int next = (g_klog_qw + 1) % KLOG_QSIZE;
    if (next != g_klog_qr) { g_klog_q[g_klog_qw] = id; g_klog_qw = next; }
    pthread_mutex_unlock(&g_klog_lock);
}

static uint64_t klog_dequeue_ms(int ms) {
    for (int t = 0; t < ms; t += 100) {
        pthread_mutex_lock(&g_klog_lock);
        if (g_klog_qw != g_klog_qr) {
            uint64_t id = g_klog_q[g_klog_qr];
            g_klog_qr = (g_klog_qr + 1) % KLOG_QSIZE;
            pthread_mutex_unlock(&g_klog_lock);
            return id;
        }
        pthread_mutex_unlock(&g_klog_lock);
        usleep(100000);
    }
    return 0;
}

/* ── Notification ─────────────────────────────────────────────────────── */
typedef struct { char _unk[45]; char message[3075]; } NotifyRequest;
static void notify(const char *fmt, ...) {
    NotifyRequest req; va_list ap;
    memset(&req, 0, sizeof(req));
    va_start(ap, fmt); vsnprintf(req.message, sizeof(req.message), fmt, ap); va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ── klog capture thread ──────────────────────────────────────────────── */
static uint64_t parse_hex_str(const char *s) {
    uint64_t v = 0;
    while (*s) {
        char c = *s++;
        if      (c >= '0' && c <= '9') v = (v<<4)|(c-'0');
        else if (c >= 'a' && c <= 'f') v = (v<<4)|(c-'a'+10);
        else if (c >= 'A' && c <= 'F') v = (v<<4)|(c-'A'+10);
        else break;
    }
    return v;
}

static void parse_klog_line(const char *line) {
    if (!strstr(line, "DEVICE_ADDED"))        return;
    if (!strstr(line, "subType:22"))          return;
    if (!strstr(line, "capabilityBattery:0")) return;
    const char *p = strstr(line, "DeviceId:0x");
    if (!p) p = strstr(line, "deviceId=0x");
    if (!p) return;
    p += 11; /* DeviceId:0x and deviceId=0x are both 11 characters. */
    uint64_t id = parse_hex_str(p);
    if (!id) return;
    gp_log("klog: VDA device 0x%llx\n", (unsigned long long)id);
    klog_enqueue(id);
}

static void *klog_capture_thread(void *arg) {
    (void)arg;
    char buf[512], line[1024]; size_t line_len = 0;
    int fd = open("/dev/klog", O_RDONLY);
    if (fd < 0) {
        gp_log("klog: /dev/klog errno=%d, trying TCP 127.0.0.1:3232\n", errno);
        struct sockaddr_in sin; int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return NULL;
        memset(&sin,0,sizeof(sin)); sin.sin_family=AF_INET;
        sin.sin_addr.s_addr=inet_addr("127.0.0.1"); sin.sin_port=htons(3232);
        for (int r=0; r<10; r++) {
            if (connect(sock,(struct sockaddr*)&sin,sizeof(sin))==0){fd=sock;gp_log("klog: connected\n");break;}
            usleep(500000);
        }
        if (fd<0){close(sock);return NULL;}
    }
    while (1) {
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len < 0) { if (errno==EINTR) continue; break; }
        if (len == 0) { usleep(10000); continue; }
        for (ssize_t i=0; i<len; i++) {
            char c = buf[i];
            if (c=='\n'||line_len>=sizeof(line)-1){line[line_len]='\0';parse_klog_line(line);line_len=0;}
            else if (c!='\r') line[line_len++]=c;
        }
    }
    close(fd); return NULL;
}

/* ── VDI injection ────────────────────────────────────────────────────── */
static pthread_mutex_t g_inject_lock = PTHREAD_MUTEX_INITIALIZER;
static void inject_pad_locked(int slot, const ScePadData *pad);
/* The USB reader and the PS Move shifter both feed the wheel's virtual pad. */
static void inject_pad(int slot, const ScePadData *pad) {
    pthread_mutex_lock(&g_inject_lock);
    inject_pad_locked(slot, pad);
    pthread_mutex_unlock(&g_inject_lock);
}
static void inject_pad_locked(int slot, const ScePadData *pad) {
    int32_t h = g_slots[slot].handle;
    int is_wheel = g_slots[slot].vid == WHEEL_VID && g_slots[slot].pid == WHEEL_PID;
    if (h < 0 || !g_slots[slot].vdi_ready) {
        if (is_wheel) wheel_diagnostics_not_sent();
        return;
    }
    int vr = scePadVirtualDeviceInsertData(h, pad);
    if (is_wheel) wheel_diagnostics_injection(vr);
    uint32_t n = ++g_slots[slot].inject_count;
    if ((n % 600) == 0)
        gp_log("slot[%d] VDI #%u ret=0x%08x\n", slot, n, (uint32_t)vr);
    static int vdi_err_logged = 0;
    if (vr != 0 && !vdi_err_logged) { gp_log("VDI error 0x%08x\n",(uint32_t)vr); vdi_err_logged=1; }
}

/* ── ugen detection ───────────────────────────────────────────────────── */
#define VID_NATIVE  0x2dc8u
#define PID_NATIVE  0x310bu
#define VID_SWITCH  0x057eu
#define PID_SWITCH  0x2009u
#define VID_XBOX    0x045eu
#define PID_XBOX    0x02eau
#define VID_WHEEL   WHEEL_VID
#define PID_WHEEL   WHEEL_PID

static const char *UGEN_PATHS[] = {
    "/dev/ugen2.2","/dev/ugen2.3","/dev/ugen2.4","/dev/ugen2.5",
    "/dev/ugen2.6","/dev/ugen2.7","/dev/ugen2.8","/dev/ugen2.9",
    "/dev/ugen1.2","/dev/ugen1.3","/dev/ugen1.4","/dev/ugen1.5",
    "/dev/ugen0.2","/dev/ugen0.3","/dev/ugen0.4","/dev/ugen0.5",
    "/dev/ugen2.1","/dev/ugen1.1","/dev/ugen0.1",
};
#define N_UGEN_PATHS ((int)(sizeof(UGEN_PATHS)/sizeof(UGEN_PATHS[0])))

/* Probe one ugen path to identify controller type.
 * Returns 1 with vid/pid set, 0 if not a known controller. */
static int probe_one_path(const char *path, uint16_t *out_vid, uint16_t *out_pid) {
    if (strncmp(path, "/dev/ugen", 9) != 0) return 0;

    int fd = open(path, O_RDWR|O_NONBLOCK);
    if (fd < 0) return 0;

    /* Verifica o dispositivo via USB_GET_DEVICE_DESC */
    struct usb_device_descriptor desc;
    if (ioctl(fd, USB_GET_DEVICE_DESC, &desc) == 0) {
        uint16_t v = UGETW(desc.idVendor);
        uint16_t p = UGETW(desc.idProduct);

        /* Se for Sony (DualSense, PSVR, dongles oficiais) -> NUNCA ENCOSTAR! */
        if (v == 0x054cu) {
            close(fd);
            return 0;
        }

        /* Se for o volante Thrustmaster Ferrari 458 Spider */
        if (v == VID_WHEEL && p == PID_WHEEL) {
            gp_log("probe: %s -> Thrustmaster Ferrari 458 Spider (044F:B671)!\n", path);
            *out_vid = VID_WHEEL;
            *out_pid = PID_WHEEL;
            close(fd);
            return 1;
        }

        /* Qualquer outro dispositivo USB que nao seja o volante -> IGNORAR com seguranca */
        close(fd);
        return 0;
    }

    close(fd);
    return 0;
}

/* ── Create VDA and force_bind for a slot ─────────────────────────────── */
static int32_t create_vda_for_slot(int slot) {
    struct { int32_t size; int32_t userId; int32_t pad[6]; } vdp;
    const int32_t SEN = (int32_t)0xDEADBEEFu;
    memset(&vdp,0,sizeof(vdp)); vdp.size=sizeof(vdp); vdp.userId=1;
    for(int k=0;k<6;k++) vdp.pad[k]=SEN;

    int ret = scePadVirtualDeviceAddDevice(&vdp, VIRTUAL_DEVICE_TYPE_DUALSENSE);
    gp_log("slot[%d] VDA ret=0x%08x\n", slot, (uint32_t)ret);

    int32_t handle = (ret > 0) ? ret : -1;
    for(int k=0;k<6;k++){
        if(vdp.pad[k]!=SEN && vdp.pad[k]>0){if(handle<0)handle=vdp.pad[k];break;}
    }

    uint64_t dev_id = klog_dequeue_ms(10000);
    if (dev_id) {
        handle = (int32_t)(dev_id & 0xffffffffu);
        int br = shellui_pad_force_bind(dev_id, g_inject_uid);
        gp_log("slot[%d] force_bind(0x%llx, 0x%08x) ret=%d\n",
               slot, (unsigned long long)dev_id, (uint32_t)g_inject_uid, br);
    } else if (handle >= 0) {
        gp_log("slot[%d] klog timeout — using direct handle %d\n", slot, handle);
    } else {
        gp_log("slot[%d] Cannot identify the new virtual device — refusing unrelated handles\n", slot);
    }
    if (handle>=0)
        gp_log("slot[%d] VDI handle=0x%x ready\n", slot, (uint32_t)handle);
    else
        gp_log("slot[%d] ERROR: no VDA handle\n", slot);
    return handle;
}

/* ── USB HID thread ───────────────────────────────────────────────────── */
/* Single-session: receives slot+path+vid+pid, runs until disconnect, then exits.
 * Manager thread handles re-detection after exit. */

typedef struct {
    int      slot;
    char     dev_path[32];
    uint16_t vid, pid;
} usb_thread_arg_t;

static void *usb_hid_thread(void *arg) {
    usb_thread_arg_t *targ = (usb_thread_arg_t *)arg;
    int slot = targ->slot;
    char dev_path[32]; memcpy(dev_path, targ->dev_path, sizeof(dev_path));
    uint16_t vid = targ->vid, pid = targ->pid;
    free(targ);

    struct usb_fs_endpoint eps[2];
    struct usb_fs_init     init;
    struct usb_fs_open     fs_open;
    struct usb_fs_start    start;
    struct usb_fs_complete complete;
    struct usb_fs_stop     stop;
    struct usb_fs_close    fs_close;
    struct usb_fs_uninit   uninit;
    uint8_t  buf[64];
    void    *buffers[1]; uint32_t lengths[1];
    int fd = -1, out_opened = 0;
    int usb_ready_notified = 0;

    gp_log("slot[%d] USB thread: %s VID=0x%04x PID=0x%04x\n",
           slot, dev_path, vid, pid);

    /* ── Thrustmaster Ferrari 458 Spider ──────────────────────────────── */
    if (pid == PID_WHEEL) {
        wheel_diagnostics_event(WHEEL_DETECTED, 0);
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { wheel_diagnostics_event(WHEEL_ERROR, errno); gp_log("slot[%d] Wheel open fail errno=%d\n", slot, errno); goto exit_slot; }

        /* Recheck after opening: a hotplug can change a probed ugen path. */
        struct usb_device_descriptor wheel_desc;
        if (ioctl(fd, USB_GET_DEVICE_DESC, &wheel_desc) != 0 ||
            UGETW(wheel_desc.idVendor) != VID_WHEEL || UGETW(wheel_desc.idProduct) != PID_WHEEL) {
            wheel_diagnostics_event(WHEEL_ERROR, errno ? errno : EIO);
            gp_log("slot[%d] Wheel identity changed — leaving USB drivers attached\n", slot);
            close(fd);
            goto exit_slot;
        }

        wheel_diagnostics_event(WHEEL_USB_OPEN, 0);
        { int ii; for(ii=0;ii<4;ii++){int i2=ii; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2);} }
        usleep(120000);
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            wheel_diagnostics_event(WHEEL_ERROR, errno ? errno : EIO);
            gp_log("slot[%d] Wheel FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=WHEEL_EP_IN;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
            wheel_diagnostics_event(WHEEL_ERROR, errno ? errno : EIO);
            gp_log("slot[%d] Wheel IN fail errno=%d\n",slot,errno);
            goto uninit_exit;
        }
        gp_log("slot[%d] Wheel IN ep=0x%02x ok maxpkt=%u\n",
               slot, WHEEL_EP_IN, (unsigned)fs_open.max_packet_length);

        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=WHEEL_EP_OUT;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        gp_log("slot[%d] Wheel OUT ep=0x%02x opened=%d\n", slot, WHEEL_EP_OUT, out_opened);

        int handshake_result = out_opened ? wheel_gip_handshake(fd, eps) : -errno;
        if (!out_opened || handshake_result != 0) {
            wheel_diagnostics_event(WHEEL_ERROR, handshake_result < 0 ? -handshake_result : EIO);
            gp_log("slot[%d] Wheel handshake failed — retrying detection\n", slot);
            goto reinit;
        }
        goto main_loop;
    }

    /* ── Xbox One: single-pass ─────────────────────────────────────────── */
    if (pid == PID_XBOX) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] Xbox open fail errno=%d\n", slot, errno); goto exit_slot; }

        { int ii; for(ii=0;ii<4;ii++){int i2=ii; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2);} }
        usleep(120000); /* hub settle: give USB hub time to propagate detach */
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] Xbox FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=XBOX_EP_IN;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
            gp_log("slot[%d] Xbox IN fail errno=%d\n",slot,errno);
            goto uninit_exit;
        }
        gp_log("slot[%d] Xbox IN ep=0x%02x ok maxpkt=%u\n",
               slot, XBOX_EP_IN, (unsigned)fs_open.max_packet_length);

        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=XBOX_EP_OUT;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        gp_log("slot[%d] Xbox OUT ep=0x%02x opened=%d\n", slot, XBOX_EP_OUT, out_opened);

        if (!out_opened || xbox_gip_handshake(fd, eps) != 0) goto reinit;
        goto main_loop;
    }

    /* ── Nintendo: two-pass ────────────────────────────────────────────── */
    fd = open(dev_path, O_RDWR);
    if (fd < 0) { gp_log("slot[%d] open fail errno=%d\n", slot, errno); goto exit_slot; }

    memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
    init.pEndpoints=eps; init.ep_index_max=1;
    if (ioctl(fd,USB_FS_INIT,&init)!=0){
        gp_log("slot[%d] Nintendo FS_INIT p1 fail\n",slot); close(fd); goto exit_slot;
    }
    { int i0=0,i1=1; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i0); ioctl(fd,USB_IFACE_DRIVER_DETACH,&i1); }
    memset(&fs_open,0,sizeof(fs_open));
    fs_open.ep_index=0; fs_open.ep_no=0x81; fs_open.max_bufsize=64; fs_open.max_frames=1;
    if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
        gp_log("slot[%d] Nintendo IN p1 fail errno=%d\n",slot,errno); goto uninit_exit;
    }
    gp_log("slot[%d] Nintendo p1 IN ok maxpkt=%u\n",slot,(unsigned)fs_open.max_packet_length);
    memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
    close(fd); fd=-1;

    /* Pass 2: retry DETACH+OPEN up to 5 times to beat usb_hid0 re-attach
     * (real Switch Pro is claimed by PS5 native HID driver after probe releases it) */
    { int detach_try;
      for (detach_try = 0; detach_try < 5; detach_try++) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] reopen fail attempt %d\n",slot,detach_try); goto exit_slot; }
        { int i0=0,i1=1,i2=2;
          ioctl(fd,USB_IFACE_DRIVER_DETACH,&i0);
          ioctl(fd,USB_IFACE_DRIVER_DETACH,&i1);
          ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2); }
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            close(fd); fd=-1; usleep(50000); continue;
        }
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=0x81; fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) break; /* claimed it */
        memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
        close(fd); fd=-1;
        gp_log("slot[%d] Nintendo p2 OPEN retry %d errno=%d\n",slot,detach_try,errno);
        usleep(50000);
      }
      if (fd < 0) { gp_log("slot[%d] Nintendo p2 give up\n",slot); goto exit_slot; }
    }
    gp_log("slot[%d] Nintendo p2 IN ok maxpkt=%u\n",slot,(unsigned)fs_open.max_packet_length);
    if (fs_open.max_packet_length != 64){ gp_log("slot[%d] wrong maxpkt, reinit\n",slot); goto reinit; }

    buffers[0]=buf; lengths[0]=64;
    eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
    eps[0].timeout=200; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

    /* 8BitDo uses ep=0x02; real Nintendo Switch Pro Controller uses ep=0x01 */
    memset(&fs_open,0,sizeof(fs_open));
    fs_open.ep_index=1; fs_open.ep_no=0x02; fs_open.max_bufsize=64; fs_open.max_frames=1;
    out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
    if (!out_opened) {
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=0x01; fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        if (out_opened) gp_log("slot[%d] Nintendo OUT ep=0x01 (real Switch Pro)\n", slot);
    }
    gp_log("slot[%d] Nintendo OUT opened=%d\n", slot, out_opened);
    if (out_opened) {
        usb_send_cmd(fd,&eps[1],0x80,0x02); usleep(30000);
        usb_send_cmd(fd,&eps[1],0x80,0x04); usleep(50000);
        gp_log("slot[%d] Nintendo [80 02]+[80 04] sent\n", slot);
    }

main_loop: ;
    int hs_state = (pid==PID_XBOX) ? HS_STREAMING : HS_WAIT_81_01;
    uint8_t nintendo_seq = 1;

    while (1) {
        memset(buf,0,64);
        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths;
        eps[0].aFrames=0; eps[0].status=0;

        memset(&start,0,sizeof(start)); start.ep_index=0;
        if (ioctl(fd,USB_FS_START,&start)!=0) {
            if (pid == PID_WHEEL) wheel_diagnostics_event(WHEEL_ERROR, errno);
            if (errno==EBUSY){
                memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
                gp_log("slot[%d] USB endpoint busy — reinitializing\n", slot);
                goto reinit;
            } else if (errno==ENXIO||errno==ENOTTY){
                gp_log("slot[%d] START errno=%d — device gone\n",slot,errno); goto reinit;
            } else {
                gp_log("slot[%d] START fatal errno=%d\n",slot,errno); goto reinit;
            }
            continue;
        }

        int ok=0, cerr=0, cw=0;
        for(cw=0;cw<(eps[0].timeout+50)*2;cw++){
            memset(&complete,0,sizeof(complete)); complete.ep_index=0;
            if(ioctl(fd,USB_FS_COMPLETE,&complete)==0){
                if (complete.ep_index != 0) continue;
                ok=1;break;
            }
            cerr=errno;
            if(cerr==ENXIO||cerr==ENOTTY){if (pid == PID_WHEEL) wheel_diagnostics_event(WHEEL_ERROR, cerr);gp_log("slot[%d] COMPLETE errno=%d — gone\n",slot,cerr);goto reinit;}
            if(cerr!=EBUSY && cerr!=EINTR) break;
            usleep(500);
        }
        if(!ok){
            if (pid == PID_WHEEL) wheel_diagnostics_event(WHEEL_ERROR, cerr ? cerr : ETIMEDOUT);
            memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
            gp_log("slot[%d] USB completion wait failed errno=%d\n",slot,cerr);
            goto reinit;
        }
        if (eps[0].status == USB_ERR_TIMEOUT) {
            if (pid == PID_WHEEL) wheel_diagnostics_usb_poll(WHEEL_POLL_TIMEOUT);
            continue;
        }
        if (eps[0].status != USB_ERR_NORMAL_COMPLETION) {
            if (pid == PID_WHEEL) wheel_diagnostics_event(WHEEL_ERROR, eps[0].status);
            gp_log("slot[%d] USB input status=%d — reinitializing\n", slot, eps[0].status);
            goto reinit;
        }
        if(eps[0].aFrames != 1 || lengths[0]<1 || lengths[0]>sizeof(buf)) {
            if (pid == PID_WHEEL) wheel_diagnostics_usb_poll(WHEEL_POLL_DISCARDED);
            continue;
        }
        if (pid == PID_WHEEL) wheel_diagnostics_usb_poll(WHEEL_POLL_DATA);

        uint32_t len = lengths[0];

        ScePadData pad; memset(&pad,0,sizeof(pad)); pad.quat.w=1.0f;
        int injected = 0;

        if (pid == PID_WHEEL) {
            injected = wheel_handle_packet(fd, eps, buf, len, &pad);
        } else if (pid == PID_XBOX) {
            injected = xbox_handle_packet(fd, eps, buf, len, &pad);
        } else {
            injected = nintendo_handle_packet(fd, eps, buf, len, &hs_state, &nintendo_seq, &pad);
        }

        if (injected < 0) {
            if (pid == PID_WHEEL) wheel_diagnostics_event(WHEEL_ERROR, -injected);
            goto reinit;
        }
        if (injected > 0) {
            if (!usb_ready_notified) {
                notify("Ghostcontrol: slot[%d] streaming — volante ativo", slot);
                usb_ready_notified = 1;
            }
            /* First real button press or steering confirms the assignment — release the gate
             * so the manager can start the next controller's dialog. */
            if (!g_slots[slot].confirmed && (pad.buttons != 0 || pad.analogButtons.r2 > 30 || pad.analogButtons.l2 > 30 || pad.leftStick.x != 128)) {
                g_slots[slot].confirmed = 1;
                if (g_assign_slot == slot) g_assign_slot = -1;
                gp_log("slot[%d] assignment confirmed (activity)\n", slot);
            }
            inject_pad(slot, &pad);
        }
    }

reinit:
    if (usb_ready_notified) notify("Ghostcontrol: slot[%d] controller disconnected", slot);
    memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
    if (out_opened) {
        memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=1; ioctl(fd,USB_FS_CLOSE,&fs_close);
    }
    memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=0; ioctl(fd,USB_FS_CLOSE,&fs_close);
uninit_exit:
    memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
    close(fd);

exit_slot:
    if (pid == PID_WHEEL) { wheel_reset_state(); wheel_diagnostics_event(WHEEL_DISCONNECTED, 0); }
    gp_log("slot[%d] USB thread exiting — freeing slot\n", slot);
    /* The PS Move thread also injects: no insert may race the deletion. */
    pthread_mutex_lock(&g_inject_lock);
    g_slots[slot].vdi_ready = 0;
    scePadVirtualDeviceDeleteDevice(g_slots[slot].handle);
    pthread_mutex_unlock(&g_inject_lock);
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].handle    = -1;
    g_slots[slot].vdi_ready = 0;
    g_slots[slot].usb_active= 0;
    g_slots[slot].dev_path[0] = '\0';
    g_slots[slot].confirmed = 0;
    if (g_assign_slot == slot) g_assign_slot = -1;
    pthread_mutex_unlock(&g_slot_lock);
    return NULL;
}

/* ── Controller manager thread ────────────────────────────────────────── */
static void *controller_manager_thread(void *arg) {
    (void)arg;
    int scan = 0;
    gp_log("Manager thread started (MAX_SLOTS=%d)\n", MAX_SLOTS);

    while (1) {
        for (int i = 0; i < N_UGEN_PATHS; i++) {
            const char *path = UGEN_PATHS[i];

            /* Skip non-ugen paths */
            if (strncmp(path, "/dev/ugen", 9) != 0) continue;

            /* Serialize assignment: if a controller is still awaiting the user's
             * button press to confirm its dialog, do not start another one. */
            if (g_assign_slot >= 0) break;

            /* Skip if already claimed by an active slot */
            int busy = 0;
            pthread_mutex_lock(&g_slot_lock);
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].usb_active && strcmp(g_slots[s].dev_path, path) == 0) {
                    busy = 1; break;
                }
            }
            pthread_mutex_unlock(&g_slot_lock);
            if (busy) continue;

            /* Try to identify controller */
            uint16_t vid=0, pid=0;
            if (!probe_one_path(path, &vid, &pid)) continue;

            /* Find free slot */
            int slot = -1;
            pthread_mutex_lock(&g_slot_lock);
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].handle < 0 && !g_slots[s].usb_active) { slot=s; break; }
            }
            pthread_mutex_unlock(&g_slot_lock);

            if (slot < 0) {
                if ((scan % 5) == 0) gp_log("manager: all %d slots full\n", MAX_SLOTS);
                continue;
            }

            const char *name =
                (vid==VID_WHEEL  && pid==PID_WHEEL)  ? "Thrustmaster Ferrari 458 Spider" :
                (vid==VID_SWITCH && pid==PID_SWITCH) ? "Nintendo Switch Pro / 8BitDo" :
                (vid==VID_NATIVE && pid==PID_NATIVE) ? "8BitDo Native" :
                (vid==VID_XBOX   && pid==PID_XBOX)   ? "Xbox One S" : "Unknown";

            gp_log("manager: %s at %s → slot[%d]\n", name, path, slot);
            notify("Ghostcontrol: %s detected — assign user on screen", name);

            /* Claim the slot path before VDA so manager skips it if we retry.
             * Set the assignment gate — released when user confirms (button press). */
            pthread_mutex_lock(&g_slot_lock);
            strncpy(g_slots[slot].dev_path, path, sizeof(g_slots[slot].dev_path)-1);
            g_slots[slot].usb_active = 1; /* tentatively claimed */
            g_slots[slot].confirmed  = 0;
            g_slots[slot].vid = vid;
            g_slots[slot].pid = pid;
            pthread_mutex_unlock(&g_slot_lock);
            g_assign_slot = slot;

            /* Create VDA and force_bind (shows PS5 assignment dialog) */
            int32_t handle = create_vda_for_slot(slot);
            if (handle < 0) {
                gp_log("manager: slot[%d] VDA failed — releasing\n", slot);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].usb_active = 0;
                g_slots[slot].dev_path[0] = '\0';
                g_slots[slot].handle = -1;
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
                continue;
            }

            pthread_mutex_lock(&g_slot_lock);
            g_slots[slot].handle      = handle;
            g_slots[slot].vdi_ready   = 1;
            g_slots[slot].inject_count = 0;
            pthread_mutex_unlock(&g_slot_lock);

            /* Launch USB reader thread */
            usb_thread_arg_t *targ = malloc(sizeof(*targ));
            if (!targ) {
                gp_log("manager: malloc fail for slot[%d]\n", slot);
                scePadVirtualDeviceDeleteDevice(handle);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].handle=-1; g_slots[slot].vdi_ready=0;
                g_slots[slot].usb_active=0; g_slots[slot].dev_path[0]='\0';
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
                continue;
            }
            targ->slot = slot;
            strncpy(targ->dev_path, path, sizeof(targ->dev_path)-1);
            targ->dev_path[sizeof(targ->dev_path)-1] = '\0';
            targ->vid=vid; targ->pid=pid;

            pthread_t tid;
            if (pthread_create(&tid, NULL, usb_hid_thread, targ) != 0) {
                gp_log("manager: pthread_create fail slot[%d]\n", slot);
                free(targ);
                scePadVirtualDeviceDeleteDevice(handle);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].handle=-1; g_slots[slot].vdi_ready=0;
                g_slots[slot].usb_active=0; g_slots[slot].dev_path[0]='\0';
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
            } else {
                pthread_detach(tid);
                gp_log("manager: slot[%d] USB thread started handle=0x%x\n",
                       slot, (uint32_t)handle);
                notify("Ghostcontrol: slot[%d] ready — press a button to assign", slot);
            }
            /* One controller per scan pass — assignment gate blocks the rest
             * until the user confirms this one with a button press. */
            break;
        }

        /* Assignment timeout: if the user never presses a button, release the
         * gate after ~30s so the queue does not stall forever. */
        static int assign_wait = 0;
        if (g_assign_slot >= 0) {
            if (++assign_wait > 15) {  /* 15 * 2s = 30s */
                gp_log("manager: assignment timeout slot[%d] — releasing gate\n", g_assign_slot);
                g_assign_slot = -1;
                assign_wait = 0;
            }
        } else {
            assign_wait = 0;
        }

        scan++;
        if ((scan % 5) == 0) {
            /* Log active slots every 10s */
            int active = 0;
            for (int s = 0; s < MAX_SLOTS; s++)
                if (g_slots[s].usb_active) active++;
            if (active == 0 && (scan % 10) == 0)
                gp_log("manager: scan #%d — no controllers\n", scan);
        }
        usleep(2000000);
    }
    return NULL;
}

/* ── PS Move over Bluetooth (/dev/hid) ────────────────────────────────── */
/* ioctl layouts match the RPCS3 Move handler that is verified on this console. */
typedef struct { uint32_t handle, arg1, arg2; } hid_open_req;
typedef struct { uint32_t type, handle; } hid_close_req;
typedef struct { uint8_t report_id, r, g, b, pad0, pad1, rumble, pad2; } hid_led_payload;
typedef struct { uint32_t driver_handle, pad; void *p_payload; } hid_set_led_req;
typedef struct { uint32_t driver_handle, pad0; void *buffer; uint32_t max_reports, pad1; int32_t *p_count; } hid_read_req;
#define HID_IOCTL_OPEN  0x800c4802u
#define HID_IOCTL_CLOSE 0x80084803u
#define HID_IOCTL_LED   0x80104839u
#define HID_IOCTL_READ  0xc0204834u
#define MOVE_LED_R 255
#define MOVE_LED_G 0
#define MOVE_LED_B 255

static int move_set_led(int fd, uint32_t dh, uint8_t r, uint8_t g, uint8_t b) {
    hid_led_payload led = { 1, r, g, b, 0, 0, 0, 0 };
    hid_set_led_req req = { dh, 0, &led };
    return ioctl(fd, HID_IOCTL_LED, &req);
}

static int move_read(int fd, uint32_t dh, uint8_t *buf, uint32_t max_reports) {
    int32_t count = 0;
    hid_read_req req = { dh, 0, buf, max_reports, 0, &count };
    return ioctl(fd, HID_IOCTL_READ, &req);
}

/* Scan Bluetooth HID handles 1..8 (0 is the DualSense). Accept a handle when the
 * LED command succeeds or input reports already arrive. Returns driver handle or 0. */
static uint32_t move_connect(int fd) {
    for (uint32_t h = 1; h <= 8; h++) {
        hid_open_req oreq = { h, 3, 1 };
        int dh = ioctl(fd, HID_IOCTL_OPEN, &oreq);
        if (dh <= 0) continue;
        int r = move_set_led(fd, (uint32_t)dh, MOVE_LED_R, MOVE_LED_G, MOVE_LED_B);
        int rr = 0;
        if (r != 0) { uint8_t probe[64]; rr = move_read(fd, (uint32_t)dh, probe, 1); }
        if (r == 0 || rr > 0) {
            /* Other Bluetooth pads also answer here: keep the handle only if it streams
             * Move reports (id 0x01 and an accelerometer reading near 1 g). */
            static uint8_t probe[16 * MOVE_KERNEL_REPORT_SIZE];
            int is_move = 0, seen = 0;
            for (int tries = 0; tries < 100 && !is_move; tries++) {
                int n = move_read(fd, (uint32_t)dh, probe, 16);
                for (int i = 0; i < n && i < 16; i++) {
                    move_input in;
                    seen++;
                    if (!move_parse_report(probe + i * MOVE_KERNEL_REPORT_SIZE, &in)) continue;
                    float g = sqrtf(in.accel[0] * in.accel[0] + in.accel[1] * in.accel[1] + in.accel[2] * in.accel[2]);
                    if (g > 0.5f && g < 2.0f) { is_move = 1; break; }
                }
                usleep(10000);
            }
            if (is_move) {
                gp_log("move: PS Move on BT handle %u (dh=0x%08x led=%d)\n", h, (uint32_t)dh, r);
                return (uint32_t)dh;
            }
            gp_log("move: BT handle %u is not a PS Move (%d reports, id=0x%02x); skipping\n", h, seen, probe[MOVE_HID_OFFSET]);
        }
        hid_close_req creq = { 3, (uint32_t)dh };
        ioctl(fd, HID_IOCTL_CLOSE, &creq);
    }
    return 0;
}

/* Wheel slot that has a virtual pad, or -1. */
static int wheel_slot(void) {
    int found = -1;
    pthread_mutex_lock(&g_slot_lock);
    for (int s = 0; s < MAX_SLOTS && found < 0; s++)
        if (g_slots[s].vid == WHEEL_VID && g_slots[s].pid == WHEEL_PID && g_slots[s].handle >= 0 && g_slots[s].vdi_ready) found = s;
    pthread_mutex_unlock(&g_slot_lock);
    return found;
}

/* Hold the extra buttons on the wheel's virtual pad and send the result now. */
static void shifter_send(uint32_t extra) {
    int slot = wheel_slot();
    ScePadData pad;
    /* Keep the wheel report as composed, timestamp included: the wheel's own reports
     * carry 0, and a mix of clocks on one virtual pad was ignored by Project CARS. */
    if (wheel_compose_pad(extra, &pad) && slot >= 0) inject_pad(slot, &pad);
}

#define SHIFT_PRESS_US 150000u  /* about as long as a quick pull of the real paddle */

/* PS Move shifter / handbrake lever: reads the Move, recognises shifts (or the lever
 * pull) and holds the chosen buttons on the wheel's virtual pad. Does nothing to the
 * game while the option is off, and only scans Bluetooth while it is in use. */
static void *move_shifter_thread(void *arg) {
    (void)arg;
    int fd = -1;
    uint32_t dh = 0, extra = 0, shift_bit = 0;
    uint64_t last_input = 0, last_led = 0, last_log = 0, release_at = 0;
    static uint8_t reports[16 * MOVE_KERNEL_REPORT_SIZE];

    gp_log("move: shifter thread started\n");
    while (1) {
        if (fd < 0) {
            fd = open("/dev/hid", O_RDWR);
            if (fd < 0) { gp_log("move: /dev/hid errno=%d\n", errno); usleep(2000000); continue; }
        }
        if (!dh) {
            /* Leave Bluetooth pads alone unless the Move is in use or being set up. */
            if (!shifter_wanted(shifter_now_us())) { usleep(1000000); continue; }
            dh = move_connect(fd);
            if (!dh) { usleep(1000000); continue; }
            last_input = last_led = shifter_now_us();
            notify("Cambio PS Move conectado");
        }

        int r = move_read(fd, dh, reports, 16);
        uint64_t t = shifter_now_us();
        shifter_status st;
        shifter_get(&st, t);
        wheel_config_t cfg;
        wheel_config_get(&cfg);
        uint32_t handbrake = 0;
        int lever_held = 0;   /* the button chosen to go with the handbrake pull */
        if (r > 0) {
            int n = r > 16 ? 16 : r;
            for (int i = 0; i < n; i++) {
                move_input cur;
                if (!move_parse_report(reports + i * MOVE_KERNEL_REPORT_SIZE, &cur)) continue;
                /* Two IMU samples per report at ~87 Hz. */
                shifter_buttons b = { cur.trigger, cur.t_button, cur.move, cur.cross, cur.circle, cur.square, cur.triangle };
                /* Either sample can complete a shift: keep both results. */
                int first = shifter_update(cur.accel1, cur.gyro1, &b, t - 5750);
                int kind = shifter_update(cur.accel, cur.gyro, &b, t);
                if (!kind) kind = first;
                if (!kind) kind = shifter_take_test();
                if (kind && st.enabled) {
                    /* A new shift replaces one still being held. */
                    /* Games that ignore the paddles get the buttons chosen on /cambio. */
                    if (kind == SHIFT_UP) shift_bit = st.up_button ? st.up_button : cfg.btn_paddle_r;
                    else shift_bit = st.down_button ? st.down_button : cfg.btn_paddle_l;
                    release_at = t + SHIFT_PRESS_US;
                    wheel_virtual_shift(kind == SHIFT_UP);
                    shifter_count_sent();
                }
                if (st.enabled && !st.lever && st.handbrake && cur.trigger > 100) handbrake = st.handbrake;
                lever_held = shifter_hold_pressed(st.lever_hold, &b);
            }
            last_input = t;
            if (t - last_log > 10000000u) { last_log = t; gp_log("move: tilt=%.1f level=%.1f ups=%u downs=%u enabled=%d\n", st.tilt_deg, st.level_deg, st.ups, st.downs, st.enabled); }
        } else {
            handbrake = extra & st.handbrake;  /* no new report: keep the handbrake as it was */
            if (r < 0 && (errno == ENODEV || errno == EBADF)) {
                gp_log("move: read failed errno=%d; reconnecting\n", errno);
                shifter_disconnected();
                if (extra) shifter_send(extra = 0);
                dh = 0; continue;
            }
        }
        if (r > 0) {
            /* Handbrake lever: held for as long as the Move stays pulled back. */
            shifter_status now_st;
            shifter_get(&now_st, t);
            if (now_st.enabled && now_st.lever && now_st.lever_pulled && lever_held) handbrake = now_st.handbrake;
        }
        if (shift_bit && t >= release_at) shift_bit = 0;
        uint32_t want = shift_bit | handbrake;
        if (!st.enabled) want = 0;
        if (want != extra) shifter_send(extra = want);

        /* Keep the sphere lit; the Move turns its LED off without periodic output. */
        if (t - last_led > 500000u) { move_set_led(fd, dh, MOVE_LED_R, MOVE_LED_G, MOVE_LED_B); last_led = t; }

        if (t - last_input > 8000000u) {
            gp_log("move: no input for 8 s; reconnecting\n");
            hid_close_req creq = { 3, dh };
            ioctl(fd, HID_IOCTL_CLOSE, &creq);
            shifter_disconnected();
            if (extra) shifter_send(extra = 0);
            dh = 0;
            continue;
        }
        usleep(4000);
    }
    return NULL;
}

/* ── Credential elevation ─────────────────────────────────────────────── */
static void elevate_credentials(void) {
    pid_t p = getpid();
    uint8_t caps[16]; memset(caps,0xff,sizeof(caps));
    kernel_set_ucred_authid(p, 0x3800000000010003l);
    kernel_set_ucred_caps(p, caps);
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(void) {
    int32_t userId=-1, fgUser=-1; int ret;

    /* Keep the descriptor open: the OS releases ownership when this process exits. */
    if (mkdir(LOG_DIR, 0755) != 0 && errno != EEXIST) return 1;
    int instance_fd = instance_claim(INSTANCE_PATH, 1);
    if (instance_fd < 0) {
        notify("Ghostcontrol: cannot reserve driver instance (error %d)", errno);
        return 1;
    }
    wheel_config_load();
    wheel_reset_state();
    shifter_init();
    shifter_options_load(SHIFTER_OPTIONS_PATH);
    /* A legacy payload without the lock may still own 8088. Do not start a
     * second USB reader or terminate an unverified PID in that case. */
    if (web_server_start(8088) != 0) {
        notify("Ghostcontrol: port 8088 unavailable; stop the previous driver (error %d)", errno);
        close(instance_fd);
        return 1;
    }
    ghostpad_status_log_reset();
    gp_log("Ghost-Control v5 starting — %d slots\n", MAX_SLOTS);
    notify("Ghostcontrol by StonedModder — plug in controllers now");

    /* PID_PATH stays informational for existing tools. Never signal its value. */
    { int pfd = open(PID_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (pfd >= 0) {
          char text[32]; int n = snprintf(text, sizeof(text), "%d", (int)getpid());
          if (write(pfd, text, (size_t)n) != n) gp_log("PID file write failed\n");
          close(pfd);
      }
    }

    /* Init slots */
    for (int s = 0; s < MAX_SLOTS; s++) {
        g_slots[s].handle     = -1;
        g_slots[s].vdi_ready  = 0;
        g_slots[s].usb_active = 0;
        g_slots[s].confirmed  = 0;
        g_slots[s].dev_path[0]= '\0';
    }
    g_assign_slot = -1;

    sceUserServiceInitialize(NULL);
    sceUserServiceGetInitialUser(&userId);
    sceUserServiceGetForegroundUser(&fgUser);
    gp_log("userId=0x%08x fgUser=0x%08x\n", (uint32_t)userId, (uint32_t)fgUser);
    if ((uint32_t)userId<0x10000000u||(uint32_t)userId>0x1000000Fu) userId=0x10000000;
    g_inject_uid = (fgUser > 0) ? fgUser : userId;
    gp_log("inject_uid=0x%08x\n", (uint32_t)g_inject_uid);

    elevate_credentials();

    ret=scePadInit(); gp_log("scePadInit: 0x%08x\n", ret);
    ret=scePadSetProcessPrivilege(1); gp_log("scePadSetProcessPrivilege: 0x%08x\n", ret);

    /* Each slot deletes only its own virtual device when its USB reader exits.
     * Do not delete arbitrary handles belonging to other payloads/controllers. */

    /* Start klog capture thread first — must be running before any VDA call */
    pthread_t klog_tid;
    if (pthread_create(&klog_tid, NULL, klog_capture_thread, NULL)==0) {
        pthread_detach(klog_tid);
        gp_log("klog thread started\n");
    }
    usleep(300000); /* let klog thread connect before first VDA */

    /* Start controller manager — handles all detection, VDA creation, USB threads */
    pthread_t mgr_tid;
    if (pthread_create(&mgr_tid, NULL, controller_manager_thread, NULL)==0) {
        pthread_detach(mgr_tid);
        gp_log("Manager thread started\n");
    }
    pthread_t move_tid;
    if (pthread_create(&move_tid, NULL, move_shifter_thread, NULL)==0) {
        pthread_detach(move_tid);
        gp_log("PS Move shifter thread started\n");
    }

    gp_log("Web server configurator active on port 8088\n");

    /* Keep-alive */
    uint32_t tick = 0;
    while (1) {
        usleep(1000000);
        tick++;
        if (tick % 10 == 0) {
            int active = 0;
            for (int s = 0; s < MAX_SLOTS; s++) if (g_slots[s].usb_active) active++;
            gp_log("alive tick=%u active_slots=%d\n", tick, active);
        }
    }
    return 0;
}
