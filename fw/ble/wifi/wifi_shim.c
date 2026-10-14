/* WiFi shim: the symbols JieLi's WiFi/lwIP/WPA libraries need beyond what the BLE shim provides.
 * Organised by category; each stub does the minimum the library expects. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "shim.h"

/* ---- libc extensions the WiFi stack needs ---- */
int *__errno(void) { static int e; return &e; }
int errno;

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *a == *b)
        a++, b++;
    return n == (size_t)-1 ? 0 : (unsigned char)*a - (unsigned char)*b;
}
char *strncpy(char *d, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}
char *strcat(char *d, const char *s)
{
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) ;
    return r;
}
char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return c == 0 ? (char *)s : 0;
}
size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (*s) {
        const char *a = accept;
        int found = 0;
        while (*a) { if (*a++ == *s) { found = 1; break; } }
        if (!found) break;
        s++; n++;
    }
    return n;
}
int atoi(const char *s)
{
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}
long simple_strtol(const char *s, char **end, int base)
{
    long n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') base = 8;
        else base = 10;
    } else if (base == 16 && *s == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        n = n * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -n : n;
}
/* JieLi's bytecmp (apps/wifi_camera/wifi/rt_talk_net.c): 0 when all n bytes equal ch, else -1 */
int bytecmp(const unsigned char *p, unsigned char ch, unsigned int n)
{
    while (n--)
        if (*p++ != ch)
            return -1;
    return 0;
}

/* printf family: %s %c %d %i %u %x %X %p, width, '0', '-', l / ll */
static void put_c(char *buf, size_t sz, size_t *n, char c)
{
    if (*n + 1 < sz)
        buf[*n] = c;
    (*n)++;
}
int vsnprintf(char *buf, size_t sz, const char *f, __builtin_va_list ap)
{
    size_t n = 0;
    while (*f) {
        int width = 0, zero = 0, left = 0, ll = 0, k = 0, neg = 0;
        char t[24];
        const char *s = 0;
        unsigned long long v = 0;
        if (*f != '%') {
            put_c(buf, sz, &n, *f++);
            continue;
        }
        f++;
        for (;; f++)
            if (*f == '0') zero = 1;
            else if (*f == '-') left = 1;
            else break;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        if (*f == '.') {                          /* precision: skipped */
            f++;
            while (*f >= '0' && *f <= '9') f++;
        }
        while (*f == 'l' || *f == 'z' || *f == 'h')
            ll += *f++ == 'l';
        switch (*f) {
        case 's':
            s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            break;
        case 'c':
            t[k++] = (char)__builtin_va_arg(ap, int);
            break;
        case 'd': case 'i': {
            long long x = ll >= 2 ? __builtin_va_arg(ap, long long) : __builtin_va_arg(ap, int);
            neg = x < 0;
            v = neg ? (unsigned long long)-x : (unsigned long long)x;
            do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
            break;
        }
        case 'u': case 'x': case 'X': case 'p': {
            unsigned base = *f == 'u' ? 10u : 16u;
            const char *dg = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            v = ll >= 2 ? __builtin_va_arg(ap, unsigned long long) : __builtin_va_arg(ap, unsigned);
            do { t[k++] = dg[v % base]; v /= base; } while (v);
            break;
        }
        case '%':
            t[k++] = '%';
            break;
        default:
            if (!*f) continue;
            break;
        }
        if (*f) f++;
        {
            int len = s ? (int)strlen(s) : k + neg, pad = width > len ? width - len : 0;
            if (!left && !(zero && !s))
                while (pad > 0) { put_c(buf, sz, &n, ' '); pad--; }
            if (neg) put_c(buf, sz, &n, '-');
            if (!left)
                while (pad > 0) { put_c(buf, sz, &n, '0'); pad--; }
            if (s)
                while (*s) put_c(buf, sz, &n, *s++);
            else
                while (k) put_c(buf, sz, &n, t[--k]);
            while (pad > 0) { put_c(buf, sz, &n, ' '); pad--; }
        }
    }
    if (sz)
        buf[n < sz ? n : sz - 1] = 0;
    return (int)n;
}
int snprintf(char *buf, size_t sz, const char *f, ...)
{
    __builtin_va_list ap;
    int n;
    __builtin_va_start(ap, f);
    n = vsnprintf(buf, sz, f, ap);
    __builtin_va_end(ap);
    return n;
}
int sprintf(char *buf, const char *f, ...)
{
    __builtin_va_list ap;
    int n;
    __builtin_va_start(ap, f);
    n = vsnprintf(buf, 256, f, ap);
    __builtin_va_end(ap);
    return n;
}

/* ---- more libc for wpa_supplicant ---- */
void *memchr(const void *p, int c, size_t n)
{
    const unsigned char *s = p;
    for (; n; n--, s++)
        if (*s == (unsigned char)c)
            return (void *)s;
    return 0;
}
char *strstr(const char *h, const char *nd)
{
    size_t m = strlen(nd);
    if (!m) return (char *)h;
    for (; *h; h++)
        if (*h == *nd && !strncmp(h, nd, m))
            return (char *)h;
    return 0;
}
char *strdup(const char *s)                       /* BT's libraries brought one; the WiFi side uses it too */
{
    size_t n = strlen(s) + 1u;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}
void *realloc(void *p, size_t n)
{
    void *q;
    size_t old;
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    old = *((uint32_t *)p - 1) - 4u;              /* the heap's size word, header included (misc.c) */
    if (old >= n) return p;
    q = malloc(n);
    if (q) {
        memcpy(q, p, old);
        free(p);
    }
    return q;
}
static uint32_t rand_s = 0x2545F491u;
int rand(void)
{
    rand_s ^= rand_s << 13; rand_s ^= rand_s >> 17; rand_s ^= rand_s << 5;
    return (int)(rand_s & 0x7FFFFFFF);
}
void srand(unsigned s) { rand_s = s ? s : 1u; }
void abort(void) { shim_log("wifi: abort()"); shim_reboot(); for (;;) ; }
void qsort(void *base, size_t n, size_t w, int (*cmp)(const void *, const void *))
{
    unsigned char *b = base, tmp;
    size_t i, j, k;
    for (i = 1; i < n; i++)                       /* insertion sort: wpa sorts a handful of scan results */
        for (j = i; j && cmp(b + (j - 1) * w, b + j * w) > 0; j--)
            for (k = 0; k < w; k++) {
                tmp = b[(j - 1) * w + k];
                b[(j - 1) * w + k] = b[j * w + k];
                b[j * w + k] = tmp;
            }
}
/* newlib's ctype table: _U 1, _L 2, _N 4, _S 8, _P 16, _C 32, _X 64, _B 128; the pointer is one before */
static const char ctype_tab[1 + 256] = {
    0,
    32,32,32,32,32,32,32,32,32,40,40,40,40,40,32,32, 32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,
    -120,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16, 4,4,4,4,4,4,4,4,4,4,16,16,16,16,16,16,
    16,65,65,65,65,65,65,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,16,16,16,16,16,
    16,66,66,66,66,66,66,2,2,2,2,2,2,2,2,2, 2,2,2,2,2,2,2,2,2,2,2,16,16,16,16,32,
};
const char *__ctype_ptr__ = ctype_tab;
int if_nametoindex(const char *name) { (void)name; return 1; }
char *if_indextoname(unsigned idx, char *name) { (void)idx; strcpy(name, "wl0"); return name; }
int sscanf(const char *s, const char *f, ...)
{
    (void)s; (void)f;
    shim_log("wifi: sscanf unsupported");
    return 0;
}

/* ---- time / jiffies ---- */
volatile unsigned long jiffies;         /* 10 ms ticks (the SDK's HZ 100): shim_timers and the 1 ms ISR keep it */
uint32_t timer_get_ms(void) { return shim_ms(); }
uint32_t timer_get_sec(void) { return shim_ms() / 1000; }
uint32_t OSGetTime(void) { return shim_ms() / 10; }
int time_lapse(uint32_t *r, uint32_t ms) { uint32_t n = shim_ms(); if (n - *r >= ms) { *r = n; return 1; } return 0; }
/* JieLi's (system.a os_api.c): these sleep, they do not spin (a spin starves the scheduler: no receive) */
void udelay(uint32_t us) { os_time_dly((int)(us / 10000u)); }   /* under 10 ms: a yield */
void mdelay(uint32_t ms) { os_time_dly(ms / 10u ? (int)(ms / 10u) : 1); }
void msleep(uint32_t ms) { os_time_dly(ms / 10u ? (int)(ms / 10u) : 1); }
int t_compensate(int v) { return v; }
int t_compensate_ms(int v) { return v; }

/* ---- OS message queues (os_q_*): thin wrappers around the shim's semaphores ---- */
/* the library's OS_QUEUE is a FreeRTOS StaticQueue_t (~80 B): keep only {magic, state} in it,
 * the ring and its semaphore on the heap */
typedef struct {
    void **ring;
    uint32_t size, rd, wr;
    OS_SEM sem;
} wifi_q_t;
typedef struct { uint32_t magic; wifi_q_t *q; } wifi_qh_t;
#define Q_MAGIC 0x57514D31u
static wifi_q_t *qof(void *pevent)
{
    wifi_qh_t *h = pevent;
    return h->magic == Q_MAGIC ? h->q : 0;
}

int os_q_create(void *pevent, uint32_t size)
{
    wifi_qh_t *h = pevent;
    wifi_q_t *q = zalloc(sizeof *q);
    if (!q) return -1;
    q->ring = malloc((size ? size : 1u) * sizeof(void *));
    if (!q->ring) { free(q); return -1; }
    q->size = size ? size : 1u;
    os_sem_create(&q->sem, 0);
    h->q = q;
    h->magic = Q_MAGIC;
    return 0;
}
int os_q_del(void *pevent, uint8_t force)
{
    wifi_qh_t *h = pevent;
    wifi_q_t *q = qof(pevent);
    (void)force;
    h->magic = 0;
    if (q) { free(q->ring); free(q); }
    return 0;
}
int os_q_post(void *pevent, void *msg)
{
    wifi_q_t *q = qof(pevent);
    uint32_t f;
    if (!q) { shim_log("os_q_post: bad queue"); return -1; }
    f = shim_irq_save();
    if (q->wr - q->rd >= q->size) { shim_irq_restore(f); shim_log("os_q_post: full"); return -1; }
    q->ring[q->wr++ % q->size] = msg;
    shim_irq_restore(f);
    os_sem_post(&q->sem);
    return 0;
}
int os_q_pend(void *pevent, int timeout, void *msg)
{
    wifi_q_t *q = qof(pevent);
    uint32_t f;
    int rc;
    if (!q) { shim_log("os_q_pend: bad queue"); return -1; }
    rc = os_sem_pend(&q->sem, timeout);
    if (rc) return rc;
    f = shim_irq_save();
    if (q->wr == q->rd) { shim_irq_restore(f); return -1; }
    *(void **)msg = q->ring[q->rd++ % q->size];
    shim_irq_restore(f);
    return 0;
}
int os_q_valid(void *pevent) { return qof(pevent) != 0; }

/* ---- thread_fork / thread_kill: create tasks on the cooperative scheduler ---- */
int shim_cur_index(void);
void shim_task_set_pid(int i, int *pid);
void shim_task_kill_req(int i);
int shim_task_alive(int i);
int thread_fork(const char *name, int prio, int stk_size, uint32_t q_size,
                int *pid, void (*func)(void *), void *parm)
{
    char lb[40];
    int rc = task_create(func, parm, name);
    if (pid)
        *pid = rc == 0 ? shim_task_last() : 0;
    if (rc == 0)
        shim_task_set_pid(shim_task_last(), pid);   /* zeroed when the thread ends (KILL_WAIT waits on it) */
    snprintf(lb, sizeof lb, "fork %s p%d s%d rc%d id%d", name ? name : "?", prio, stk_size, rc,
             rc == 0 ? shim_task_last() : 0);
    shim_log(lb);
    return rc;
}
/* JieLi's (system.a os_api.c): KILL_WAIT asks and waits until the thread has ended (*pid zeroed by its
 * end), KILL_REQ only asks, KILL_FORCE deletes it; a thread killing itself just lets go of its pid */
void thread_kill(volatile int *pid, int mode)
{
    char lb[40];
    int id;
    if (!pid || !*pid)
        return;
    id = *pid;
    snprintf(lb, sizeof lb, "kill id%d m%d", id, mode);
    shim_log(lb);
    if (id == shim_cur_index()) {
        *pid = 0;
        return;
    }
    if (mode == 2 || !shim_task_alive(id)) {
        *pid = 0;
        shim_task_end(id);
        return;
    }
    shim_task_kill_req(id);
    if (mode == 1)
        return;
    {
        uint32_t t0 = shim_ms();
        while (*pid) {
            if (shim_ms() - t0 > 5000u) {         /* not JieLi's: a stuck thread would hang us for good */
                snprintf(lb, sizeof lb, "kill id%d: forced", id);
                shim_log(lb);
                *pid = 0;
                shim_task_end(id);
                break;
            }
            os_time_dly(2);
        }
    }
}
/* thread_rpc (os_api.h): start when condition() is non-zero, run func(parm), then
 * thread_complete_cb(cb_parm); the WiFi library's network_connect_thread clears its "busy" there */
typedef struct {
    int (*cond)(void);
    void (*func)(void *);
    void *parm;
    void (*cb)(void *);
    void *cb_parm;
} rpc_t;
static void rpc_entry(void *a)
{
    rpc_t r = *(rpc_t *)a;
    free(a);
    while (r.cond && !r.cond())
        os_time_dly(1);
    r.func(r.parm);
    if (r.cb)
        r.cb(r.cb_parm);
}
int thread_rpc(int (*cond)(void), const char *name, int prio, int stk_size,
               uint32_t q_size, int *pid, void (*func)(void *), void *parm,
               void (*cb)(void *), void *cb_parm)
{
    rpc_t *r = malloc(sizeof *r);
    int rc;
    if (!r)
        return -1;
    r->cond = cond;
    r->func = func;
    r->parm = parm;
    r->cb = cb;
    r->cb_parm = cb_parm;
    rc = thread_fork(name, prio, stk_size, q_size, pid, rpc_entry, r);
    if (rc)
        free(r);
    return rc;
}

/* ---- semaphore extras ---- */
int os_sem_del(OS_SEM *s, int force) { (void)force; ((shim_sem_t *)s)->magic = 0; return 0; }
int os_sem_valid(OS_SEM *s) { return ((shim_sem_t *)s)->magic == SHIM_SEM_MAGIC; }

/* HSM: the SDK's own hsm.a (build.py WIFI_LIBS); its assertions land here */
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    char lb[40];
    (void)file;
    snprintf(lb, sizeof lb, "assert %s:%d %s", func ? func : "?", line, expr ? expr : "");
    shim_log(lb);
    abort();
}

/* ---- memory: kmalloc/kfree/kzalloc ---- */
void *kmalloc(size_t n, int flags) { (void)flags; return malloc(n); }
void kfree(void *p) { free(p); }
void *kzalloc(size_t n, int flags) { (void)flags; return zalloc(n); }

/* ---- timers / timeouts ---- */
uint16_t source_timer;
void sys_timer_del(u16 id);
void usr_timeout_del(uint16_t id) { sys_timer_del(id); }

/* ---- random ---- */
uint32_t random32(void) { static uint32_t s = 0x12345678; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

/* ---- device stubs (dev_open / dev_close / dev_ioctl: the WiFi driver's "wl_hw" device) ---- */
void *dev_open(const char *name, void *arg)
{
    char lb[40];
    snprintf(lb, sizeof lb, "dev_open %s %x", name ? name : "?", (unsigned)(uintptr_t)arg);
    shim_log(lb);
    return (void *)1;
}
int dev_close(void *device) { (void)device; return 0; }
int dev_ioctl(void *device, int cmd, uint32_t arg)
{
    char lb[40];
    (void)device;
    snprintf(lb, sizeof lb, "dev_ioctl %x %x", (unsigned)cmd, (unsigned)arg);
    shim_log(lb);
    return 0;
}

/* ---- GPIO stubs ---- */
void gpio_direction_input(uint32_t gpio) { (void)gpio; }

/* ---- net event ---- */
int net_event_notify(int from, void *event) { (void)from; (void)event; return 0; }

/* ---- power / coexistence ---- */
int check_low_power_hw_if_locked(void) { return 1; }
void *low_power_get_config(void) { static uint32_t cfg[4]; return cfg; }
int low_power_check_bt_exist(void) { return 1; }
void vPortYield(void) { if (!shim_in_irq()) os_time_dly(1); }   /* in an interrupt: nothing (os.c) */

/* ---- WiFi config constants (from apps/common/net/wifi_conf.c, no-SDRAM values) ---- */
const uint8_t WL_TX_DEBUG = 0;
const uint8_t WL_RX_DEBUG = 0;
const uint8_t WL_TX_ERR_RATIO_DEBUG_SEC = 0;
const uint8_t WL_RX_ERR_RATIO_DEBUG_SEC = 0;
const uint16_t WL_RX_BACK_GROUND_ERR_CNT_PER_SECOND = 10;
const uint8_t WL_TX_PEND_DEBUG_SEC = 2;
const uint8_t WL_RX_PEND_DEBUG_SEC = 2;
const uint8_t WL_RX_OVERFLOW_DEBUG = 0;
const uint8_t RxReorderEnable = 0;
const uint32_t MAX_REORDERING_MPDU_NUM = 1;
const uint16_t MAX_CHANNEL_TIME_BSS_INFRA = 200;
const int8_t WIFI_CHANNEL_QUALITY_INDICATION_BAD = 5;
const uint16_t MAX_PACKETS_IN_QUEUE = 16;
const uint8_t IPV4_ADDR_CONFLICT_DETECT = 0;
const uint8_t ntp_get_time_init = 0;
const uint32_t rtc_update_interval = 0;
const uint8_t CONFIG_SDIO_SLAVE_MODE = 0;
const uint8_t WIFI_TX_FULL_WAIT_MODE = 1;
const uint8_t WIFI_COLD_START_FAST_CONNECTION = 1;
const uint8_t MAX_LEN_OF_BSS_TABLE = 8;              /* scan results kept (SDK no-SDRAM: 2; each in the heap) */
const uint8_t wifi_recv_poll = 0;
uint8_t wifi_lowpower_mode = 0;
const uint16_t MAX_PACKETS_IN_MCAST_PS_QUEUE = 4;
const uint16_t MAX_PACKETS_IN_PS_QUEUE = 2;
uint8_t wl_active_wait_interval_count = 0;
uint16_t wl_custom_listen_interval = 10;
uint16_t wl_default_listen_interval = 3;

/* ---- WL_STA_DAT: the WiFi driver's config string ---- */
static const char WL_STA_DAT[] =
    "Default\n"
    "MacAddress=00:00:00:00:00:00\n"
    "CountryRegion=1\n"
    "CountryRegionABand=0\n"
    "CountryCode=CN\n"
    "ChannelGeography=1\n"
    "SSID=DEFAULT_CONNECT_SSID\n"
    "NetworkType=Infra\n"
    "WirelessMode=9\n"
    "Channel=1\n"
    "BeaconPeriod=100\n"
    "TxPower=100\n"
    "BGProtection=0\n"
    "TxPreamble=1\n"
    "RTSThreshold=2347\n"
    "FragThreshold=2346\n"
    "TxBurst=0\n"
    "PktAggregate=0\n"
    "WmmCapable=1\n"
    "AckPolicy=0;0;0;0\n"
    "AuthMode=OPEN\n"
    "EncrypType=NONE\n"
    "WPAPSK=\n"
    "DefaultKeyID=1\n"
    "Key1Type=0\nKey1Str=\n"
    "Key2Type=0\nKey2Str=\n"
    "Key3Type=0\nKey3Str=\n"
    "Key4Type=0\nKey4Str=\n"
    "PSMode=CAM\n"
    "AutoRoaming=0\n"
    "RoamThreshold=70\n"
    "APSDCapable=1\n"
    "APSDAC=0;0;0;0\n"
    "HT_RDG=1\n"
    "HT_EXTCHA=0\n"
    "HT_OpMode=0\n"
    "HT_MpduDensity=4\n"
    "HT_BW=0\n"
    "HT_BADecline=0\n"
    "HT_AutoBA=1\n"
    "HT_AMSDU=0\n"
    "HT_BAWinSize=64\n"
    "HT_GI=0\n"
    "HT_MCS=33\n"
    "HT_MIMOPSMode=3\n"
    "HT_DisallowTKIP=1\n"
    "HT_STBC=0\n"
    "EthConvertMode=\n"
    "EthCloneMac=\n";

const char *GET_WL_STA_DAT(void) { return WL_STA_DAT; }
int GET_WL_STA_DAT_LEN(void) { return sizeof(WL_STA_DAT) - 1; }

/* BSS table record: saved scan result for fast reconnect */
static uint8_t bss_record[256];
static uint32_t bss_record_len;
void set_bss_table_record(char *data, uint32_t len)
{
    if (len > sizeof bss_record) len = sizeof bss_record;
    memcpy(bss_record, data, len);
    bss_record_len = len;
}
int get_bss_table_record(char *data, uint32_t len)
{
    if (!bss_record_len) return 0;
    if (len > bss_record_len) len = bss_record_len;
    memcpy(data, bss_record, len);
    return (int)len;
}
void clear_wpa_scan_record(void) { bss_record_len = 0; }

int wl_ap_init(const char *ssid, const char *pass) { (void)ssid; (void)pass; return 0; }

int lwip_low_level_inputput_filter(uint8_t *pkg, uint32_t len) { (void)pkg; (void)len; return 0; }
int lwip_low_level_output_filter(uint8_t *pkg, uint32_t len) { (void)pkg; (void)len; return 0; }
int socket_send_but_netif_busy_hook(int s, char type_udp) { (void)s; (void)type_udp; return 0; }
int wifi_recv_pkg_and_soft_filter(uint8_t *pkg, uint32_t len) { (void)pkg; (void)len; return 0; }
void set_scan_result_record(char *data, uint32_t len) { (void)data; (void)len; }
int get_stored_scan_record(char *data, uint32_t len) { (void)data; (void)len; return 0; }
int get_stored_pmk(char *pw, char *ssid, int ssidlen, char *out) { (void)pw; (void)ssid; (void)ssidlen; (void)out; return 0; }
void set_stored_pmk(char *pw, char *ssid, int ssidlen, char *out) { (void)pw; (void)ssid; (void)ssidlen; (void)out; }
void wsc_sta_config_set_ssid_pwd(const char *ssid, const char *pwd, int sl, int pl) { (void)ssid; (void)pwd; (void)sl; (void)pl; }
const char *GET_WL_AP_DAT(void) { return ""; }
int GET_WL_AP_DAT_LEN(void) { return 0; }
int wl_set_wifi_channel(int ch) { (void)ch; return 0; }
int wifi_get_rf_trim_data(void *info, int size);
int wifi_set_rf_trim_data(void *info, int size);

/* ---- localtime_r / mktime: lwIP's SNTP needs them ---- */
struct tm_shim { int sec, min, hour, mday, mon, year, wday, yday, isdst; };
void *localtime_r(const void *t, void *result)
{
    struct tm_shim *r = (struct tm_shim *)result;
    memset(r, 0, sizeof *r);
    r->year = 126; r->mday = 1;
    return r;
}
uint32_t mktime(void *tm) { (void)tm; return 0; }
int local_time_init_flag;
