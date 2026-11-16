/* puglik: the phone link over BLE (NimBLE). No iPhone: the phone is Android with Gadgetbridge, to which Pip is a
   "Bangle.js" (it is named "Bangle.js Pip" and has the Nordic UART service): Gadgetbridge writes lines of JavaScript
   to the UART (notifications, calls, music, the time, weather, alarms) and Pip answers with JSON lines; ui/gb.c is
   the protocol, this file moves the bytes. The phone pairs once (numeric comparison: the same 6 digits on both screens).
   To a computer Pip is a mouse, page keys and media keys (HID): the remote app and the music app. Connections: the
   phone, the debug page and a computer. Who is who: one that writes to the UART is the phone, one that writes to the
   debug service within 4 s is the debug page, one that pairs and stays silent is the computer; a computer once seen
   is known by its address. Everything the UI sees goes through phone.h, like the simulator's fake phone. */
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "gb.h"
#include "phone.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "shell.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "ble";
void ble_store_config_init(void);

static void uuid(ble_uuid128_t *u, const char *s) /* "6E400001-B5A3-..." -> little-endian bytes */
{
    u->u.type = BLE_UUID_TYPE_128;
    int n = 15;
    for (; *s && n >= 0; s++) {
        if (*s == '-') continue;
        unsigned v;
        sscanf(s, "%2x", &v);
        u->value[n--] = (uint8_t)v;
        s++;
    }
}

/* ---------- connection state: the phone's place ---------- */
static struct {
    uint8_t own_addr;
    uint16_t conn;
    bool connected, numcmp, ready;
} b = {.conn = BLE_HS_CONN_HANDLE_NONE};

static void advertise(void);
static void phone_idle(uint16_t conn); /* puglik: the phone's link at its idle pace */
static void host_take(uint16_t conn);
static void press_media(uint16_t usage);
static uint16_t host_conn = BLE_HS_CONN_HANDLE_NONE; /* the computer */
static uint16_t pair_conn = BLE_HS_CONN_HANDLE_NONE; /* the connection the numeric comparison is for */

static void ui(void (*fn)(void *), void *arg) /* run UI code under the LVGL lock */
{
    ui_lock();
    fn(arg);
    ui_unlock();
}

static void ui_link_off(void *a) { (void)a; phone_link(PHONE_OFF, 0); }
static void ui_link_pair(void *a) { phone_link(PHONE_PAIRING, *(uint32_t *)a); }

/* ---------- Gadgetbridge: the Nordic UART ---------- */
static ble_uuid128_t NUS, NUS_RX, NUS_TX;
static uint16_t nus_tx, gb_conn = BLE_HS_CONN_HANDLE_NONE;

static void gb_send_line(const char *s) /* to the phone, in notifications as big as the link allows */
{
    uint16_t c = gb_conn;
    if (c == BLE_HS_CONN_HANDLE_NONE) return;
    int n = (int)strlen(s), mtu = ble_att_mtu(c) - 3;
    if (mtu < 20) mtu = 20;
    for (int at = 0; at < n; at += mtu) {
        int k = n - at < mtu ? n - at : mtu;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s + at, (uint16_t)k);
        if (!om || ble_gatts_notify_custom(c, nus_tx, om)) return;
    }
}

static void gb_set_time(int64_t unix_s, int zone)
{
    struct timeval tv = {.tv_sec = (time_t)unix_s};
    if (unix_s > 1735689600) settimeofday(&tv, NULL); /* after 2025-01-01: main.c then counts real time */
    if (zone >= -720 && zone <= 840) tz_min = zone;
    ESP_LOGI(TAG, "time from the phone: %lld, UTC%+d min", (long long)unix_s, zone);
}
static int gb_tz(void) { return tz_min; }

typedef struct { const char *d; int n; } chunk_t;
static void ui_gb_feed(void *a) { chunk_t *c = a; gb_feed(c->d, c->n); }
static void ui_gb_up(void *a) { (void)a; gb_link(true); }
static void ui_gb_down(void *a) { (void)a; gb_link(false); }

static void gb_take(uint16_t conn);
static int nus_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (conn != gb_conn) gb_take(conn);
    char d[256];
    uint16_t n = 0;
    ble_hs_mbuf_to_flat(ctxt->om, d, sizeof d, &n);
    chunk_t c = {d, n};
    ui(ui_gb_feed, &c);
    return 0;
}

static void command(int cmd, uint32_t uid)
{
    if (gb_command(cmd, uid)) return; /* notifications, calls and music: Gadgetbridge (ui/gb.c) */
    switch (cmd) {
    case PHONE_PAIR_OK:
    case PHONE_PAIR_CANCEL:
        if (b.numcmp) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = cmd == PHONE_PAIR_OK};
            ble_sm_inject_io(pair_conn, &io);
            b.numcmp = false;
        }
        break;
    case PHONE_SHUTTER: /* "volume up" is the camera's shutter on Android too: to whoever has Pip as a keyboard */
        if (host_conn != BLE_HS_CONN_HANDLE_NONE) press_media(0xE9);
        break;
    }
}

/* ---------- who we have met: computers, by identity address, kept in NVS ---------- */
#define HOSTS_MAX 4
static ble_addr_t hosts[HOSTS_MAX]; /* computers seen before (identity addresses), newest first, kept in NVS */
static int hosts_n;

static bool addr_in(uint16_t conn, const ble_addr_t *list, int n)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d)) return false;
    for (int i = 0; i < n; i++)
        if (!ble_addr_cmp(&list[i], &d.peer_id_addr)) return true;
    return false;
}

static bool addr_add(uint16_t conn, ble_addr_t *list, int *n, const char *key) /* true if it is new */
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) || addr_in(conn, list, *n)) return false;
    memmove(&list[1], &list[0], sizeof list[0] * (HOSTS_MAX - 1));
    list[0] = d.peer_id_addr;
    if (*n < HOSTS_MAX) (*n)++;
    nvs_handle_t h;
    if (nvs_open("keypet", NVS_READWRITE, &h) != ESP_OK) return true;
    if (nvs_set_blob(h, key, list, sizeof list[0] * (size_t)*n) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return true;
}


/* ---------- the phone's place: encrypted, it has NUS_WAIT_MS to write to the UART, or it is a computer ---------- */
#define NUS_WAIT_MS 5000
static struct ble_npl_callout nus_wait;
static void phone_or_computer(struct ble_npl_event *ev)
{
    (void)ev;
    if (!b.connected || b.conn == gb_conn) return;
    uint16_t c = b.conn; /* silent: a computer, for the remote; the phone's place is free again */
    b = (typeof(b)){.own_addr = b.own_addr, .conn = BLE_HS_CONN_HANDLE_NONE};
    ui(ui_link_off, NULL);
    host_take(c);
}

static void step(void)
{
    b.ready = true;
    phone_idle(b.conn); /* puglik */
    if (b.conn != gb_conn) ble_npl_callout_reset(&nus_wait, ble_npl_time_ms_to_ticks32(NUS_WAIT_MS));
}

static void gb_take(uint16_t conn) /* it wrote to the UART: Gadgetbridge, the phone */
{
    gb_conn = conn;
    ble_npl_callout_stop(&nus_wait);
    if (conn == host_conn) host_conn = BLE_HS_CONN_HANDLE_NONE; /* taken for a computer a moment ago: it is the phone */
    if (!b.connected || b.conn != conn) b = (typeof(b)){.own_addr = b.own_addr, .conn = conn, .connected = true, .ready = true};
    ESP_LOGI(TAG, "Gadgetbridge connected");
    static const gb_host_t host = {gb_send_line, gb_set_time, gb_tz};
    gb_start(&host);
    ui(ui_gb_up, NULL);
    phone_idle(conn);
    advertise(); /* the debug page or a computer may still come */
}


/* ---------- our own services: the battery, the debug link, who we are, the computer's HID, the UART ---------- */
/* The computer's, in its own service: a mouse with three buttons, x, y and a wheel (relative, report id 1), a keyboard
   (id 2: modifiers, reserved, six keys) and media keys (id 3: one consumer usage). */
#define MOUSE_MAP                                                                                                        \
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, /* generic desktop: mouse, application */                                      \
        0x85, 0x01,                     /*   report id 1 */                                                            \
        0x09, 0x01, 0xA1, 0x00,         /*   pointer, physical */                                                      \
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, /* buttons 1..3 */                                                         \
        0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, /* + padding */ \
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, /* x, y, wheel: -127..127, relative */                         \
        0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0
static const uint8_t host_map[] = {
    MOUSE_MAP,
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, /* generic desktop: keyboard, application */
    0x85, 0x02,                         /*   report id 2 */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, /* modifiers */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, /*   reserved byte */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, /* six keys */
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, /* consumer control, application */
    0x85, 0x03,                         /*   report id 3 */
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, /* one usage */
    0xC0,
};
#define HOST_REPORT(id) ((void *)(intptr_t)(id)) /* the access callback's arg in the computer's service: its report id */
static uint16_t battery_handle, mouse_handle, keys_handle, media_handle;

static int read_flat(struct ble_gatt_access_ctxt *ctxt, const void *v, int n)
{
    return os_mbuf_append(ctxt->om, v, n) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static int hid_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t id = ble_uuid_u16(ctxt->chr ? ctxt->chr->uuid : ctxt->dsc->uuid);
    static const uint8_t info[4] = {0x11, 0x01, 0x00, 0x02}, ref[2] = {1, 1}, zero = 0, protocol = 1;
    static const uint8_t pnp[7] = {0x02, 0x5E, 0x04, 0x01, 0x00, 0x01, 0x00}; /* USB vendor id space, product 1, v1 */
    static const uint8_t still[8] = {0}, host_ref[3][2] = {{1, 1}, {2, 1}, {3, 1}};
    static const int still_n[3] = {4, 8, 2}; /* mouse, keyboard, media */
    int rid = (int)(intptr_t)arg; /* 0: the phone's service */
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR || ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC) return 0; /* control point */
    switch (id) {
    case 0x2A4A: return read_flat(ctxt, info, 4);                            /* HID information */
    case 0x2A4B:                                                             /* report map */
        return read_flat(ctxt, host_map, sizeof host_map);
    case 0x2A4D: return rid ? read_flat(ctxt, still, still_n[rid - 1]) : read_flat(ctxt, &zero, 1); /* the report: nothing pressed */
    case 0x2908: return read_flat(ctxt, rid ? host_ref[rid - 1] : ref, 2);   /* report reference: id, input */
    case 0x2A4E: return read_flat(ctxt, &protocol, 1);                        /* protocol mode: report */
    case 0x2A50: return read_flat(ctxt, pnp, 7);                              /* PnP id */
    case 0x2A29: return read_flat(ctxt, "KeyPet", 6);                         /* manufacturer */
    case 0x2A19: {                                                            /* battery level */
        int pct = shell_battery_level(NULL);
        uint8_t v = pct < 0 ? 100 : (uint8_t)pct;
        return read_flat(ctxt, &v, 1);
    }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int dbg_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);
static ble_uuid128_t DBG, DBG_RX, DBG_TX;
static uint16_t dbg_tx;

#define HOST_INPUT(id, handle) /* an input report of the computer's service, with its reference (id, input) */             \
    {.uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access, .arg = HOST_REPORT(id), .val_handle = &handle,            \
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,                                        \
     .descriptors = (struct ble_gatt_dsc_def[]){{.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, \
                                                 .access_cb = hid_access, .arg = HOST_REPORT(id)}, {0}}}
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180F),
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = hid_access, .val_handle = &battery_handle,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY}, {0}}},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &DBG.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &DBG_RX.u, .access_cb = dbg_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
         {.uuid = &DBG_TX.u, .access_cb = dbg_access, .val_handle = &dbg_tx, .flags = BLE_GATT_CHR_F_NOTIFY}, {0}}},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180A),
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ},
         {.uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ}, {0}}},
    /* the computer's */
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812),
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = hid_access, .arg = HOST_REPORT(1), .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
         {.uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = hid_access, .arg = HOST_REPORT(1), .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
         HOST_INPUT(1, mouse_handle),
         HOST_INPUT(2, keys_handle),
         HOST_INPUT(3, media_handle),
         {.uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = hid_access, .arg = HOST_REPORT(1), .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
         {.uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = hid_access, .arg = HOST_REPORT(1), .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP},
         {0}}},
    /* last: Gadgetbridge's Nordic UART (RX: it writes to us, TX: we notify it) */
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &NUS.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &NUS_RX.u, .access_cb = nus_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
         {.uuid = &NUS_TX.u, .access_cb = nus_access, .val_handle = &nus_tx, .flags = BLE_GATT_CHR_F_NOTIFY}, {0}}},
    {0},
};

/* ---------- the computer: the remote's pages, pointer and scroll, the music app's media keys ---------- */
static void ui_host_on(void *a) { (void)a; phone_host(HOST_ON, 0); }
static void ui_host_off(void *a) { (void)a; phone_host(HOST_OFF, 0); }
static void ui_host_pair(void *a) { phone_host(HOST_PAIRING, *(uint32_t *)a); }


static void hosts_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof hosts;
    if (nvs_open("keypet", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "hosts", hosts, &n) == ESP_OK) hosts_n = (int)(n / sizeof hosts[0]);
    nvs_close(h);
}

static void host_remember(uint16_t conn) { addr_add(conn, hosts, &hosts_n, "hosts"); }

/* ---------- puglik: how often the radio talks ----------
   The original kept every link at the phone's or computer's own pace and advertised fast forever. Now: links idle
   slowly and speed up only while it matters (the pointer and the wheel of the remote), and advertising is fast only
   for 30 s after a link drops, the boot or a wake, then once a second. puglik_bt_save = false: the original pace. */
bool puglik_bt_save = true;
/* units: interval 1.25 ms, timeout 10 ms. The phone: 120-150 ms, may skip 4 events (a notification waits < 1 s);
   inside iOS's rules (min >= 15 ms, max >= min + 15 ms, latency <= 30, timeout <= 6 s, max * (latency + 1) <= 2 s) */
static const struct ble_gap_upd_params LINK_PHONE_IDLE = {.itvl_min = 96, .itvl_max = 120, .latency = 4, .supervision_timeout = 600};
static const struct ble_gap_upd_params LINK_HOST_IDLE = {.itvl_min = 24, .itvl_max = 40, .latency = 4, .supervision_timeout = 400};
static const struct ble_gap_upd_params LINK_HOST_FAST = {.itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 400};
#define HOST_FAST_MS 10000 /* the remote's pointer and wheel keep the computer's link fast this long after the last move */
#define ADV_FAST_US 30000000LL
static int64_t adv_fast_until; /* advertise fast until then */
static bool host_fast;
static struct ble_npl_callout host_slow_co;

static void link_speed(uint16_t conn, const struct ble_gap_upd_params *p, const char *what)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    int rc = ble_gap_update_params(conn, p);
    ESP_LOGI(TAG, "link %s: %.1f-%.1f ms, latency %d (%d)", what, p->itvl_min * 1.25f, p->itvl_max * 1.25f, p->latency, rc);
}

static void adv_fast(void) /* someone may be looking for us now: advertise fast for a while */
{
    adv_fast_until = esp_timer_get_time() + ADV_FAST_US;
    if (ble_gap_adv_active()) ble_gap_adv_stop(); /* restarted fast by the caller's advertise() */
}

static void host_slow(struct ble_npl_event *ev)
{
    (void)ev;
    if (!host_fast) return;
    host_fast = false;
    link_speed(host_conn, &LINK_HOST_IDLE, "computer idle");
}

static void phone_idle(uint16_t conn)
{
    if (puglik_bt_save) link_speed(conn, &LINK_PHONE_IDLE, "phone idle");
}

static void host_busy(void) /* the pointer or the wheel moved: fast now, slow again HOST_FAST_MS later */
{
    if (!puglik_bt_save) return;
    if (!host_fast) host_fast = true, link_speed(host_conn, &LINK_HOST_FAST, "computer fast");
    ble_npl_callout_reset(&host_slow_co, ble_npl_time_ms_to_ticks32(HOST_FAST_MS));
}

static void host_take(uint16_t conn)
{
    if (conn == gb_conn) return; /* puglik: it writes to the UART: the phone, not a computer */
    host_conn = conn;
    ESP_LOGI(TAG, "a computer connected: Pip is its remote");
    host_remember(conn); /* next time it is known at once, before it reads the report map */
    /* 7.5..15 ms between packets: a pointer that keeps up with the hand (puglik: only while it moves, see host_busy) */
    host_fast = !puglik_bt_save;
    link_speed(conn, puglik_bt_save ? &LINK_HOST_IDLE : &LINK_HOST_FAST, puglik_bt_save ? "computer idle" : "computer");
    ui(ui_host_on, NULL);
    advertise(); /* the debug page may still come */
}

static void host_report(uint16_t handle, const uint8_t *r, int n)
{
    ble_gatts_notify_custom(host_conn, handle, ble_hs_mbuf_from_flat(r, (uint16_t)n));
}

static void mouse(int buttons, int dx, int dy, int wheel)
{
    uint8_t r[4] = {(uint8_t)buttons, (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy, (uint8_t)(int8_t)wheel};
    host_report(mouse_handle, r, 4);
}

static void press_key(uint8_t usage) /* a keyboard key, down and up */
{
    uint8_t r[8] = {0, 0, usage};
    host_report(keys_handle, r, 8);
    r[2] = 0;
    host_report(keys_handle, r, 8);
}

static void press_media(uint16_t usage) /* a media key, down and up */
{
    uint8_t r[2] = {usage & 255, usage >> 8};
    host_report(media_handle, r, 2);
    r[0] = r[1] = 0;
    host_report(media_handle, r, 2);
}

static int clamp127(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

/* HOST_SCROLL > 0 is the page going down. A notch "up" (+1) does that under macOS's natural scrolling, its default;
   with natural scrolling off (and on Windows) make this -1. */
#define WHEEL_SIGN 1

static int held; /* the mouse buttons held for a drag */

static void host_cmd(int what, int dx, int dy) /* from the UI: the remote and the music apps */
{
    if (host_conn == BLE_HS_CONN_HANDLE_NONE || !mouse_handle) return;
    switch (what) {
    case HOST_NEXT: press_key(0x4E); break; /* Page Down: the next slide in Keynote, PowerPoint, Google Slides, a PDF, a page */
    case HOST_PREV: press_key(0x4B); break; /* Page Up */
    case HOST_CLICK: mouse(1, 0, 0, 0), mouse(0, 0, 0, 0); break;
    case HOST_DOWN: host_busy(), held = 1, mouse(held, 0, 0, 0); break; /* a drag: every move below carries the button */
    case HOST_UP: held = 0, mouse(held, 0, 0, 0); break;
    case HOST_SCROLL:
        host_busy();
        for (int w = dy * WHEEL_SIGN; w; w -= clamp127(w)) mouse(held, 0, 0, clamp127(w));
        break;
    case HOST_MOVE:
        host_busy();
        while (dx || dy) { /* a report moves at most 127 px each way */
            int sx = clamp127(dx), sy = clamp127(dy);
            mouse(held, sx, sy, 0);
            dx -= sx, dy -= sy;
        }
        break;
    case HOST_PLAY_PAUSE: press_media(0xCD); break;
    case HOST_TRACK_NEXT: press_media(0xB5); break;
    case HOST_TRACK_PREV: press_media(0xB6); break;
    case HOST_VOL_UP: press_media(0xE9); break;
    case HOST_VOL_DOWN: press_media(0xEA); break;
    }
}

/* ---------- the debug link: tools/pipdebug.html in Chrome (Web Bluetooth), next to the iPhone ----------
   A second connection. Who is who: one that writes to the debug service within 4 s of connecting is the debug
   page; otherwise it is the phone (or the phone is already here, so it is the page). */
static uint16_t dbg_conn = BLE_HS_CONN_HANDLE_NONE, pending = BLE_HS_CONN_HANDLE_NONE;
static uint16_t pending2 = BLE_HS_CONN_HANDLE_NONE; /* came while the phone was here: the debug page or a computer */
static int64_t since[8]; /* when each connection came (handles are small numbers), for the Bluetooth page */
static struct ble_npl_callout claim;
static void advertise(void);

static void phone_take(uint16_t conn)
{
    ble_npl_callout_stop(&claim);
    pending = BLE_HS_CONN_HANDLE_NONE;
    if (b.connected && b.conn != conn) { /* the phone's place is taken: ask this one to encrypt, then it says who it is */
        pending2 = conn;
        ble_gap_security_initiate(conn);
        return;
    }
    b = (typeof(b)){.own_addr = b.own_addr, .conn = conn, .connected = true};
    ESP_LOGI(TAG, "connected");
    ble_gattc_exchange_mtu(b.conn, NULL, NULL);
    ble_gap_security_initiate(b.conn); /* bonded: just encrypts; new: the pairing dialog on the phone */
    advertise();                       /* the debug page may still come */
}
static void claim_phone(struct ble_npl_event *ev) { (void)ev; if (pending != BLE_HS_CONN_HANDLE_NONE) phone_take(pending); }

/* Who is it: devices may state their kind in GAP's Appearance (category = value >> 6: 1 phone, 2 computer).
   Without an answer: the phone's place if it is free (then the UART decides), a computer if not. */
static void decide(uint16_t c, int cat)
{
    if (c == gb_conn) { step(); return; } /* puglik: Gadgetbridge already said who it is */
    bool slot = c == b.conn || !b.connected;
    if (cat > 1 || (cat != 1 && !slot)) { /* anything that is not a phone: a Mac calls itself a "media player" (0x0280) */
        if (c == b.conn) b = (typeof(b)){.own_addr = b.own_addr, .conn = BLE_HS_CONN_HANDLE_NONE}, ui(ui_link_off, NULL);
        host_take(c);
        return;
    }
    if (!slot) { ESP_LOGW(TAG, "a second phone: left alone"); return; }
    if (c != b.conn) {
        b = (typeof(b)){.own_addr = b.own_addr, .conn = c, .connected = true};
        ble_gattc_exchange_mtu(c, NULL, NULL);
        advertise();
    }
    step(); /* puglik: then it writes to the UART (Gadgetbridge) or is a computer */
}

static uint16_t who_done = BLE_HS_CONN_HANDLE_NONE;
static int on_who(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (!err->status && attr) {
        uint16_t v = 0;
        ble_hs_mbuf_to_flat(attr->om, &v, sizeof v, NULL);
        ESP_LOGI(TAG, "it says it is %s (appearance 0x%04x)", v >> 6 == 1 ? "a phone" : v >> 6 ? "not a phone: the computer" : "nothing", v);
        who_done = conn;
        decide(conn, v >> 6);
    } else if (who_done != conn) decide(conn, -1); /* no answer */
    if (err->status) who_done = BLE_HS_CONN_HANDLE_NONE;
    return 0;
}

static void who_is(uint16_t c)
{
    if (ble_gattc_read_by_uuid(c, 1, 0xFFFF, BLE_UUID16_DECLARE(0x2A01), on_who, NULL)) decide(c, -1);
}

/* The same question asked the moment it connects, before any pairing (Appearance needs no encryption): a new
   computer then pairs as a computer, instead of being shown the iPhone's pairing screen. */
static int8_t kind[8]; /* what each connection said it is: Appearance category, 0 not yet */
static int kind_of(uint16_t c) { return c < 8 ? kind[c] : 0; }

static void as_computer(uint16_t c) /* not the phone after all: out of the phone's place, then it pairs or encrypts */
{
    if (c != pending) return;
    ble_npl_callout_stop(&claim);
    pending = BLE_HS_CONN_HANDLE_NONE, pending2 = c;
    ble_gap_security_initiate(c);
}

static int on_who_early(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status || !attr || conn >= 8) return 0;
    uint16_t v = 0;
    ble_hs_mbuf_to_flat(attr->om, &v, sizeof v, NULL);
    kind[conn] = (int8_t)(v >> 6 ? v >> 6 : -1);
    ESP_LOGI(TAG, "connecting: %s (appearance 0x%04x)", v >> 6 == 1 ? "a phone" : v >> 6 ? "not a phone: the computer" : "it does not say", v);
    if (v >> 6 > 1) as_computer(conn);
    return 0;
}

static int dbg_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (conn == pending) ble_npl_callout_stop(&claim), pending = BLE_HS_CONN_HANDLE_NONE;
    if (conn == pending2) pending2 = BLE_HS_CONN_HANDLE_NONE;
    if (dbg_conn != conn) {
        dbg_conn = conn;
        ESP_LOGI(TAG, "debug page connected");
        advertise();
    }
    static char line[200];
    static int len;
    uint8_t d[200];
    uint16_t n = 0;
    ble_hs_mbuf_to_flat(ctxt->om, d, sizeof d, &n);
    for (int i = 0; i < n; i++) {
        if (d[i] == '\n') line[len] = 0, debug_line(line), len = 0;
        else if (len < (int)sizeof line - 1) line[len++] = (char)d[i];
    }
    return 0;
}

bool ble_debug_on(void) { return dbg_conn != BLE_HS_CONN_HANDLE_NONE; }

void ble_debug_send(const char *s, int n) /* notifications as big as the link allows; waits when the buffers are full */
{
    uint16_t c = dbg_conn;
    if (c == BLE_HS_CONN_HANDLE_NONE) return;
    int mtu = ble_att_mtu(c) - 3;
    if (mtu < 20) mtu = 20;
    for (int at = 0; at < n;) {
        int k = n - at < mtu ? n - at : mtu;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s + at, (uint16_t)k);
        int rc = om ? ble_gatts_notify_custom(c, dbg_tx, om) : BLE_HS_ENOMEM;
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (rc) return; /* gone */
        at += k;
    }
}

/* ---------- GAP ---------- */
static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status) { advertise(); break; }
        if (e->connect.conn_handle < 8) since[e->connect.conn_handle] = esp_timer_get_time(), kind[e->connect.conn_handle] = 0;
        ble_gattc_read_by_uuid(e->connect.conn_handle, 1, 0xFFFF, BLE_UUID16_DECLARE(0x2A01), on_who_early, NULL);
        if (b.connected || pending != BLE_HS_CONN_HANDLE_NONE) { /* the debug page (it writes to us) or a computer */
            pending2 = e->connect.conn_handle;
            ble_gap_security_initiate(pending2); /* bonded: it just encrypts, then says who it is */
            advertise();
            break;
        }
        pending = e->connect.conn_handle; /* the phone, unless it turns out to be the debug page */
        ble_npl_callout_reset(&claim, ble_npl_time_ms_to_ticks32(4000));
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE: /* puglik: the fast window ran out (or a link came): carry on at the slow pace */
        advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        adv_fast(); /* puglik: whoever left may come straight back */
        if (e->disconnect.conn.conn_handle == gb_conn) { /* puglik: Gadgetbridge went */
            gb_conn = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "Gadgetbridge left");
            ui(ui_gb_down, NULL);
        }
        if (e->disconnect.conn.conn_handle == dbg_conn) {
            dbg_conn = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "debug page left");
            advertise();
            break;
        }
        if (e->disconnect.conn.conn_handle == pending) { ble_npl_callout_stop(&claim), pending = BLE_HS_CONN_HANDLE_NONE; advertise(); break; }
        if (e->disconnect.conn.conn_handle == pending2) { pending2 = BLE_HS_CONN_HANDLE_NONE; advertise(); break; }
        if (e->disconnect.conn.conn_handle == host_conn) {
            host_conn = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "the computer left");
            ui(ui_host_off, NULL);
            advertise();
            break;
        }
        ESP_LOGI(TAG, "disconnected (%d)", e->disconnect.reason);
        b.connected = false, b.conn = BLE_HS_CONN_HANDLE_NONE;
        ui(ui_link_off, NULL);
        advertise();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        bool computer = e->passkey.conn_handle == pending2 || kind_of(e->passkey.conn_handle) > 1;
        if (computer) as_computer(e->passkey.conn_handle);
        else if (e->passkey.conn_handle == pending) phone_take(pending);
        pair_conn = e->passkey.conn_handle;
        if (e->passkey.params.action == BLE_SM_IOACT_NUMCMP) { /* both screens show it; the top button confirms */
            b.numcmp = true;
            uint32_t code = e->passkey.params.numcmp;
            ui(computer ? ui_host_pair : ui_link_pair, &code);
        } else if (e->passkey.params.action == BLE_SM_IOACT_DISP) { /* the other side asks to type ours */
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000};
            ble_sm_inject_io(e->passkey.conn_handle, &io);
            uint32_t code = io.passkey;
            ui(computer ? ui_host_pair : ui_link_pair, &code);
        }
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        uint16_t c = e->enc_change.conn_handle;
        if (c == host_conn) break; /* the computer again */
        if (e->enc_change.status) {
            ESP_LOGI(TAG, "encryption failed");
            if (c == b.conn) ui(ui_link_off, NULL);
            if (c == pending2) pending2 = BLE_HS_CONN_HANDLE_NONE, ui(ui_host_off, NULL);
            break;
        }
        if (c == pending) phone_take(pending); /* the debug page never asks for encryption */
        if (c == pending2) pending2 = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "encryption on");
        if (kind_of(c) > 0) decide(c, kind_of(c)); /* it said so when it connected */
        else who_is(c);                         /* a phone or a computer: it says so itself */
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: { /* the phone forgot us: forget it too and pair again */
        struct ble_gap_conn_desc d;
        if (!ble_gap_conn_find(e->repeat_pairing.conn_handle, &d)) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    }
    return 0;
}

/* ---------- the Bluetooth page (phone.c phone_bt_*): who is on which link, a reconnect, forgetting everyone ---------- */
static void bt_peer(phone_bt_t *s, uint16_t c, int role)
{
    struct ble_gap_conn_desc d;
    if (c == BLE_HS_CONN_HANDLE_NONE || s->n >= 5 || ble_gap_conn_find(c, &d)) return;
    int8_t rssi = 0;
    ble_gap_conn_rssi(c, &rssi);
    phone_peer_t *p = &s->peer[s->n++];
    *p = (phone_peer_t){.role = role, .rssi = rssi, .secs = c < 8 ? (int)((esp_timer_get_time() - since[c]) / 1000000) : 0,
                        .encrypted = d.sec_state.encrypted, .bonded = d.sec_state.bonded};
    if (role == PEER_PHONE) p->ready = b.ready, p->ancs = p->ams = p->cts = c == gb_conn; /* Gadgetbridge: all three */
}

static void bt_query(phone_bt_t *s)
{
    memset(s, 0, sizeof *s);
    bt_peer(s, b.conn, PEER_PHONE);
    bt_peer(s, host_conn, PEER_COMPUTER);
    bt_peer(s, dbg_conn, PEER_DEBUG); /* may be the computer's own link: Chrome on the same Mac */
    bt_peer(s, pending, PEER_UNKNOWN);
    bt_peer(s, pending2, PEER_UNKNOWN);
    s->advertising = ble_gap_adv_active();
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &s->bonds);
}

static void bt_act(int what)
{
    if (what == BT_RECONNECT_PHONE) { /* drop the phone's link: iOS comes straight back and everything is looked up anew */
        if (b.connected) ble_gap_terminate(b.conn, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    /* forget everyone: every link down, every bond, and who was a phone or a computer */
    const uint16_t cs[] = {b.conn, host_conn, dbg_conn, pending, pending2};
    for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++)
        if (cs[i] != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(cs[i], BLE_ERR_REM_USER_CONN_TERM);
    ble_store_clear();
    hosts_n = 0;
    nvs_handle_t h;
    if (nvs_open("keypet", NVS_READWRITE, &h) == ESP_OK) nvs_erase_key(h, "hosts"), nvs_erase_key(h, "phones") /* the original's */, nvs_commit(h), nvs_close(h);
    ESP_LOGI(TAG, "forgot every bonded device");
}

static bool up, quiet; /* the host started; asleep in energy saving: off the air */

void ble_quiet(bool q)
{
    if (!up) return;
    quiet = q;
    if (!q) { adv_fast(), advertise(); ESP_LOGI(TAG, "awake: advertising again"); return; }
    ble_gap_adv_stop();
    const uint16_t cs[] = {b.conn, host_conn, dbg_conn, pending, pending2}; /* the iPhone reconnects by itself when we are back */
    for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++)
        if (cs[i] != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(cs[i], BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGI(TAG, "asleep: Bluetooth off the air");
}

static void advertise(void)
{
    if (quiet) return; /* a link that just went down would call us back on air */
    /* puglik: named "Bangle.js Pip" - Gadgetbridge takes a device for a Bangle.js by its name - with the HID service
       for computers; the scan response lists the Nordic UART Gadgetbridge looks for */
    static const uint8_t adv[] = {2, 0x01, 0x06,       /* flags: general discoverable, LE only */
                                  3, 0x03, 0x12, 0x18, /* services: HID */
                                  3, 0x19, 0xC1, 0x03, /* appearance: keyboard */
                                  14, 0x09, 'B', 'a', 'n', 'g', 'l', 'e', '.', 'j', 's', ' ', 'P', 'i', 'p'}; /* name */
    uint8_t rsp[18] = {17, 0x07}; /* complete list of 128-bit services */
    memcpy(rsp + 2, NUS.value, 16);
    ble_gap_adv_set_data(adv, sizeof adv);
    ble_gap_adv_rsp_set_data(rsp, sizeof rsp);
    if (ble_gap_adv_active()) return;
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int32_t ms = BLE_HS_FOREVER;
    if (puglik_bt_save) { /* puglik: 30 ms while someone may be looking for us (ends with ADV_COMPLETE), then 1022.5 ms */
        int64_t left = adv_fast_until - esp_timer_get_time();
        bool fast = left > 100000;
        p.itvl_min = p.itvl_max = fast ? 48 : 1636; /* 0.625 ms units; 1022.5 ms is one of Apple's recommended values */
        if (fast) ms = (int32_t)(left / 1000);
    }
    int rc = ble_gap_adv_start(b.own_addr, NULL, ms, &p, gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "advertising failed: %d", rc);
}

/* Bump when the services table changes: phones and computers bonded to the old one are told once (Service Changed, on
   their next connection) to look at our services again instead of trusting the handles they cached. */
#define GATT_LAYOUT 3 /* 2: the computer's service gained the keyboard and media reports; 3 (puglik): no phone keyboard, the UART */

static void gatt_layout_check(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("keypet", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u8(h, "gatt", &v);
    if (v != GATT_LAYOUT) {
        ble_svc_gatt_changed(0x0001, 0xFFFF);
        if (nvs_set_u8(h, "gatt", GATT_LAYOUT) == ESP_OK) nvs_commit(h);
        ESP_LOGI(TAG, "services changed (%d -> %d): bonded peers will look again", v, GATT_LAYOUT);
    }
    nvs_close(h);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &b.own_addr);
    gatt_layout_check();
    advertise();
    ESP_LOGI(TAG, "advertising as Pip");
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(void)
{
    uuid(&NUS, "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"); /* Nordic UART: Gadgetbridge's Bangle.js link */
    uuid(&NUS_RX, "6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
    uuid(&NUS_TX, "6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
    uuid(&DBG, "50495044-4247-4B45-5950-455400000001");
    uuid(&DBG_RX, "50495044-4247-4B45-5950-455400000002");
    uuid(&DBG_TX, "50495044-4247-4B45-5950-455400000003");
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "NimBLE did not start"); return; }
    ble_npl_callout_init(&nus_wait, nimble_port_get_dflt_eventq(), phone_or_computer, NULL); /* puglik */
    ble_npl_callout_init(&claim, nimble_port_get_dflt_eventq(), claim_phone, NULL);
    ble_npl_callout_init(&host_slow_co, nimble_port_get_dflt_eventq(), host_slow, NULL); /* puglik */
    adv_fast_until = esp_timer_get_time() + ADV_FAST_US;                                  /* puglik: fast after the boot */
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO; /* numeric comparison: secure, and one button press */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("Bangle.js Pip"); /* puglik: Gadgetbridge knows a Bangle.js by its name */
    ble_svc_gap_device_appearance_set(0x03C1); /* keyboard */
    if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) ESP_LOGE(TAG, "HID services did not register");
    ble_store_config_init(); /* bonds in NVS: pair once */
    phone_on_command(command);
    phone_on_bt(bt_query, bt_act);
    phone_on_host(host_cmd);
    hosts_load(); /* the computers we know */
    nimble_port_freertos_init(host_task);
    up = true;
}
