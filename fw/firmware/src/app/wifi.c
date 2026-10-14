/* WiFi station on JieLi's libraries (wl_wifi_sta_sfc, lwip_2_1_3_sfc, wpasupplicant, hsm) and the shim:
 * on / off, a scan list, joining, and saved networks (ssid + password, most recent first, in flash).
 * Passwords arrive from the web page (web/wifi.html) over USB-MIDI. Part of the unity build (main_wifi.c). */
#include "../../../ble/shim/shim.h"

/* ---- the library's API (include_lib/net/wifi/wifi_connect.h) */
enum { STA_MODE = 1, AP_MODE, SMP_CFG_MODE, MP_TEST_MODE, P2P_MODE, NONE_MODE };
enum {
    WIFI_EVENT_MODULE_INIT, WIFI_EVENT_MODULE_START, WIFI_EVENT_MODULE_STOP,
    WIFI_EVENT_MODULE_START_ERR,
    WIFI_EVENT_AP_START, WIFI_EVENT_AP_STOP,
    WIFI_EVENT_STA_START, WIFI_EVENT_STA_STOP,
    WIFI_EVENT_STA_SCAN_COMPLETED,
    WIFI_EVENT_STA_CONNECT_SUCC,
    WIFI_EVENT_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID,
    WIFI_EVENT_STA_CONNECT_ASSOCIAT_FAIL,
    WIFI_EVENT_STA_CONNECT_ASSOCIAT_TIMEOUT,
    WIFI_EVENT_STA_DISCONNECT,
    WIFI_EVENT_SMP_CFG_START, WIFI_EVENT_SMP_CFG_STOP,
    WIFI_EVENT_SMP_CFG_TIMEOUT, WIFI_EVENT_SMP_CFG_COMPLETED,
    WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC,
    WIFI_EVENT_STA_NETWORK_STACK_DHCP_TIMEOUT,
};
struct wifi_store_info {
    int mode;
    u8 pwd[2][64];
    u8 ssid[2][33];
    int p2p_role;
    u8 sta_cnt;
    u8 connect_best_network;
} __attribute__((packed));
struct wifi_scan_ssid_info {
    char ssid[32];
    unsigned int ssid_len;
    unsigned char mac_addr[6];
    char rssi, snr, rssi_db, rssi_rsv;
    unsigned int channel_number;
    unsigned char SignalStrength, SignalQuality, SupportedRates[16];
    char enc;
};
struct netif_info { u32 ip, gw, netmask; };
extern int wifi_on(void);
extern int wifi_off(void);
extern int wifi_is_on(void);
extern int wifi_enter_sta_mode(char *sta_ssid, char *sta_pwd);
extern void wifi_set_event_callback(int (*cb)(void *, int));
extern void wifi_set_store_ssid_cnt(u32 cnt);
extern void wifi_set_sta_connect_timeout(int sec);
extern int wifi_set_default_mode(struct wifi_store_info *parm, char force, char store);
extern char wifi_get_rssi(void);
extern int wifi_scan_req(void);
extern struct wifi_scan_ssid_info *wifi_get_scan_result(u32 *ssid_cnt);
extern void wifi_clear_scan_result(void);
extern void lwip_get_netif_info(u8 is_wireless, struct netif_info *info);
extern int snprintf(char *buf, unsigned long sz, const char *f, ...);
extern const char *os_current_task(void);
extern int os_taskq_pend(const char *fmt, int *argv, int argc);

/* the station's MAC: the BT address kept in syscfg (ble.c), made a locally administered unicast one;
 * without it the library joins as 00:00:00:00:00:00 */
int init_net_device_mac_addr(char *macaddr, char ap_mode)
{
    const uint8_t *b = bt_get_mac_addr();
    unsigned i;
    (void)ap_mode;
    for (i = 0; i < 6u; i++)
        macaddr[i] = (char)b[i];
    macaddr[0] = (char)(((uint8_t)macaddr[0] & 0xFC) | 0x02);
    macaddr[5] ^= 0x01;
    return 0;
}

/* ---- saved networks (OBJ_WIFI), most recent first */
#define NET_N 6
typedef struct { char ssid[33], pass[64]; } net_t;
/* + the radio's station: the place (a country and a name: the table may change under an index) and Radio
 * Garden's station id, with its name for the screen before the place's list is fetched again */
static struct {
    uint32_t magic;
    net_t n[NET_N];
    uint16_t last;                                /* (the old table's station index, unused) */
    uint8_t playing, rsv;
    uint16_t country, rsv2;
    char place[32], st_id[12], st_title[32];
} nets;
/* the struct before the places (its size, padding included): the networks load, the station starts empty */
#define NETS_OLD_SIZE ((__builtin_offsetof(__typeof__(nets), country) + 3u) & ~3u)
#define NETS_MAGIC 0x57494631u                    /* "WIF1" */

static int w_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static void w_cpy(char *d, const char *s, unsigned max)
{
    unsigned i;
    for (i = 0; i + 1u < max && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}
static void nets_load(void)
{
    int n;
    memset(&nets, 0, sizeof nets);
    n = plat_store_load(OBJ_WIFI, &nets, sizeof nets);
    if ((n != (int)sizeof nets && n != (int)NETS_OLD_SIZE) || nets.magic != NETS_MAGIC) {
        memset(&nets, 0, sizeof nets);
        nets.magic = NETS_MAGIC;
    }
    if (n == (int)NETS_OLD_SIZE)                  /* the networks kept; the station starts empty */
        memset((uint8_t *)&nets + __builtin_offsetof(__typeof__(nets), country), 0,
               sizeof nets - __builtin_offsetof(__typeof__(nets), country));
}
static void nets_save(void) { plat_store_save(OBJ_WIFI, &nets, sizeof nets); }
static int nets_count(void)
{
    int i;
    for (i = 0; i < NET_N && nets.n[i].ssid[0]; i++)
        ;
    return i;
}
static int nets_find(const char *ssid)
{
    int i;
    for (i = 0; i < NET_N && nets.n[i].ssid[0]; i++)
        if (w_eq(nets.n[i].ssid, ssid))
            return i;
    return -1;
}
/* to the front (added, or joined): the others move down, the oldest falls off */
static void nets_front(const char *ssid, const char *pass)
{
    net_t e;
    int i = nets_find(ssid);
    memset(&e, 0, sizeof e);
    w_cpy(e.ssid, ssid, sizeof e.ssid);
    w_cpy(e.pass, pass, sizeof e.pass);
    if (i < 0)
        i = NET_N - 1;
    for (; i > 0; i--)
        nets.n[i] = nets.n[i - 1];
    nets.n[0] = e;
    nets_save();
}
static void nets_del(int i)
{
    if (i < 0 || i >= NET_N)
        return;
    for (; i < NET_N - 1; i++)
        nets.n[i] = nets.n[i + 1];
    memset(&nets.n[NET_N - 1], 0, sizeof nets.n[0]);
    nets_save();
}

/* ---- state */
enum { WS_OFF, WS_STARTING, WS_IDLE, WS_JOINING, WS_DHCP, WS_CONNECTED, WS_FAILED };
static volatile uint8_t wifi_st;
static uint8_t wifi_powered;                      /* wifi_on() done, wifi_off() not since */
static char wifi_ip_str[20];
static char wifi_ssid[33];                        /* the network joined / being joined */
static char wifi_why[24];                         /* WS_FAILED: what went wrong */
static int8_t wifi_rssi;
static int wifi_try;                              /* auto-join: the saved network being tried */
static volatile uint8_t wifi_join_due, wifi_scan_done;

static void ip_to_str(char *buf, u32 ip)
{
    snprintf(buf, 20, "%u.%u.%u.%u", (unsigned)(ip & 255u), (unsigned)(ip >> 8 & 255u),
             (unsigned)(ip >> 16 & 255u), (unsigned)(ip >> 24));
}

/* what the screen shows of an SSID: the font is ASCII, so U+2019 (the iPhone's apostrophe) becomes ' */
static void ssid_show(char *d, const char *s, unsigned max)
{
    unsigned n = 0;
    while (*s && n + 1u < max) {
        if ((uint8_t)s[0] == 0xE2 && (uint8_t)s[1] == 0x80 && ((uint8_t)s[2] == 0x98 || (uint8_t)s[2] == 0x99)) {
            d[n++] = '\'';
            s += 3;
        } else {
            d[n++] = (uint8_t)*s >= 32 && (uint8_t)*s < 127 ? *s : '?';
            s++;
        }
    }
    d[n] = 0;
}

/* every received 802.11 frame (wifi_set_frame_cb): counted, with the receiver's frequency offset and
 * signal, for tuning the crystal (RXWI word 2: RSSI0 bits 0-7; word 3: FOFFSET bits 16-23, signed) */
extern void wifi_set_frame_cb(void (*cb)(void *rxwi, void *wh, void *data, u32 len, void *priv), void *priv);
static volatile uint32_t fr_n;
static volatile int32_t fr_foff, fr_rssi;
static void wifi_frame_cb(void *rxwi, void *wh, void *data, u32 len, void *priv)
{
    const uint32_t *w = rxwi;
    (void)wh; (void)data; (void)len; (void)priv;
    if (!w)
        return;
    fr_n++;
    fr_foff += (int8_t)(w[3] >> 16);
    fr_rssi += (int8_t)w[2];
}

static void wifi_join(const char *ssid, const char *pass)
{
    w_cpy(wifi_ssid, ssid, sizeof wifi_ssid);
    wifi_ip_str[0] = 0;
    wifi_st = WS_JOINING;
    wifi_enter_sta_mode((char *)ssid, (char *)pass);
}

static int wifi_event_cb(void *ctx, int event)
{
    char lb[40];
    (void)ctx;
    snprintf(lb, sizeof lb, "wifi ev %d st %d", event, wifi_st);
    shim_log(lb);
    switch (event) {
    case WIFI_EVENT_MODULE_INIT: {                /* not started yet: the network it starts on */
        static struct wifi_store_info m;
        memset(&m, 0, sizeof m);
        m.mode = STA_MODE;
        wifi_try = 0;
        if (nets_count()) {
            memcpy(m.ssid[0], nets.n[0].ssid, sizeof m.ssid[0] - 1u);
            memcpy(m.pwd[0], nets.n[0].pass, sizeof m.pwd[0] - 1u);
            w_cpy(wifi_ssid, nets.n[0].ssid, sizeof wifi_ssid);
        }
        wifi_set_sta_connect_timeout(20);
        wifi_set_default_mode(&m, 1, 0);
        break;
    }
    case WIFI_EVENT_MODULE_START: {               /* every rate allowed, 1M CCK .. MCS7, as the SDK's demo_wifi */
        extern void wifi_set_tx_rate_control_tab(u32 tab);
        wifi_set_tx_rate_control_tab(0xFFFFFu);
        break;
    }
    case WIFI_EVENT_STA_START:
        wifi_st = wifi_ssid[0] ? WS_JOINING : WS_IDLE;
        break;
    case WIFI_EVENT_STA_SCAN_COMPLETED:
        wifi_scan_done = 1;
        break;
    case WIFI_EVENT_STA_CONNECT_SUCC:
        wifi_st = WS_DHCP;
        break;
    case WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC: {
        struct netif_info ni = {0};
        lwip_get_netif_info(1, &ni);
        if (ni.ip)
            ip_to_str(wifi_ip_str, ni.ip);
        wifi_st = WS_CONNECTED;
        wifi_join_due = 2;                        /* the main loop: this network to the front */
        break;
    }
    case WIFI_EVENT_STA_DISCONNECT:
        if (wifi_st == WS_CONNECTED || wifi_st == WS_DHCP) {
            wifi_st = WS_JOINING;                 /* the library reconnects by itself */
            wifi_ip_str[0] = 0;
        }
        break;
    case WIFI_EVENT_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID:
        if (wifi_try + 1 < nets_count()) {        /* auto-join: the next saved network */
            wifi_try++;
            wifi_join_due = 1;
        } else {
            wifi_st = WS_FAILED;
            w_cpy(wifi_why, "not found", sizeof wifi_why);
        }
        break;
    case WIFI_EVENT_STA_CONNECT_ASSOCIAT_FAIL:
    case WIFI_EVENT_STA_CONNECT_ASSOCIAT_TIMEOUT:
        wifi_st = WS_FAILED;
        w_cpy(wifi_why, "wrong password?", sizeof wifi_why);
        break;
    case WIFI_EVENT_STA_NETWORK_STACK_DHCP_TIMEOUT:
        wifi_st = WS_FAILED;
        w_cpy(wifi_why, "no address (DHCP)", sizeof wifi_why);
        break;
    case WIFI_EVENT_MODULE_START_ERR:
        wifi_st = WS_FAILED;
        w_cpy(wifi_why, "radio did not start", sizeof wifi_why);
        break;
    default:
        break;
    }
    return 0;
}

/* the main loop's: on (and auto-join the saved networks) / off */
static void wifi_power(int on)
{
    if (on && wifi_st == WS_OFF) {
        wifi_ip_str[0] = wifi_ssid[0] = wifi_why[0] = 0;
        wifi_rssi = 0;
        wifi_st = WS_STARTING;
        if (!wifi_powered) {
            static uint8_t first = 1;
            if (first) {
                first = 0;
                shim_log_begin("wifi: start");
                shim_isr_trace.n = 0;
                shim_up();
                wifi_set_store_ssid_cnt(1);
                wifi_set_event_callback(wifi_event_cb);
            }
            wifi_powered = 1;
            {
                char lb[40];
                snprintf(lb, sizeof lb, "wifi: on rc %d", wifi_on());
                shim_log(lb);
            }
            wifi_set_frame_cb(wifi_frame_cb, 0);
        }
    } else if (!on && wifi_st != WS_OFF) {
        wifi_st = WS_OFF;
        wifi_ip_str[0] = 0;
        if (wifi_powered) {
            char lb[40];
            wifi_powered = 0;
            snprintf(lb, sizeof lb, "wifi: off rc %d", wifi_off());
            shim_log(lb);
        }
    }
}

/* a saved network (or open one) by SSID; returns 0, or -1 when it needs a password we do not have */
static int wifi_join_ssid(const char *ssid, int secured)
{
    int i = nets_find(ssid);
    if (i >= 0)
        wifi_join(ssid, nets.n[i].pass);
    else if (!secured)
        wifi_join(ssid, "");
    else
        return -1;
    return 0;
}

static void wifi_scan_collect(void);

static void wifi_poll(void)
{
    if (wifi_st == WS_OFF)
        return;
    shim_timers();
    shim_run();
    if (wifi_join_due == 1 && wifi_try < nets_count()) {
        wifi_join_due = 0;
        wifi_join(nets.n[wifi_try].ssid, nets.n[wifi_try].pass);
    } else if (wifi_join_due == 2) {
        int i = nets_find(wifi_ssid);
        wifi_join_due = 0;
        if (i > 0)
            nets_front(nets.n[i].ssid, nets.n[i].pass);
    }
    if (wifi_st == WS_CONNECTED && wifi_is_on())
        wifi_rssi = (int8_t)wifi_get_rssi();
    if (wifi_scan_done) {                         /* any scan (ours, or a join's): its results */
        wifi_scan_done = 0;
        wifi_scan_collect();
    }
}

/* ---- the scan list (the Networks page) */
#define SCAN_N 12
typedef struct { char ssid[33]; int8_t rssi; uint8_t secured, saved; } scan_t;
static scan_t scan_list[SCAN_N];
static int scan_n = -1;                           /* -1 = none yet */
/* reception score (the wire's measurements): scans seen, scans that heard a saved network, its rssi summed */
static uint32_t meas_scans, meas_hits;
static int32_t meas_rssi;
static u32 scan_last_n = 99;

static int wifi_scan_start(void)
{
    char lb[40];
    int rc = wifi_scan_req();                     /* -1 while a join runs: its own scans arrive anyway */
    snprintf(lb, sizeof lb, "scan req rc %d", rc);
    shim_log(lb);
    return rc;
}
/* after WIFI_EVENT_STA_SCAN_COMPLETED: the results, strongest first */
static void wifi_scan_collect(void)
{
    u32 n = 0, i;
    int hit = 0;
    struct wifi_scan_ssid_info *r = wifi_get_scan_result(&n);
    scan_n = 0;
    meas_scans++;
    for (i = 0; r && i < n && scan_n < SCAN_N; i++) {
        scan_t *s = &scan_list[scan_n];
        unsigned k, len = r[i].ssid_len < 32u ? r[i].ssid_len : 32u;
        int j;
        if (!len)
            continue;                             /* hidden */
        for (k = 0; k < len; k++)
            s->ssid[k] = r[i].ssid[k];
        s->ssid[k] = 0;
        for (j = 0; j < scan_n && !w_eq(scan_list[j].ssid, s->ssid); j++)
            ;
        if (j < scan_n)
            continue;                             /* the same network on another access point */
        s->rssi = (int8_t)r[i].rssi;
        if (scan_n < 3 && n != scan_last_n) {
            char lb[40];
            snprintf(lb, sizeof lb, "  %.12s r%d db%d s%u e%d", s->ssid, r[i].rssi, r[i].rssi_db,
                     r[i].SignalStrength, r[i].enc);
            shim_log(lb);
        }
        s->secured = r[i].enc != 0;
        s->saved = nets_find(s->ssid) >= 0;
        if (s->saved && !hit) {
            hit = 1;
            meas_hits++;
            meas_rssi += s->rssi;
        }
        for (j = scan_n; j > 0 && scan_list[j - 1].rssi < s->rssi; j--) {
            scan_t t = scan_list[j];
            scan_list[j] = scan_list[j - 1];
            scan_list[j - 1] = t;
        }
        scan_n++;
    }
    if (r)
        wifi_clear_scan_result();
    if (n != scan_last_n) {                       /* the log keeps changes only */
        char lb[40];
        snprintf(lb, sizeof lb, "scan: %u found, %d listed", (unsigned)n, scan_n);
        shim_log(lb);
    }
    scan_last_n = n;
}

static const char *wifi_status_str(void)
{
    static const char *const S[] = {"OFF", "STARTING", "ON", "JOINING", "GETTING IP", "CONNECTED", "FAILED"};
    return wifi_st < 7u ? S[wifi_st] : "?";
}
