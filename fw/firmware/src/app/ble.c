/* BLE experiment: JieLi's BT stack (btctrler.a, btstack.a, wl_rf_common.a: closed, linked here for a
 * private test only) on the shim in ../../shim, inside FoMni-1. Started by hand (Setup page), never
 * at boot: a crash restarts the FM-1 without Bluetooth. Part of the unity build (main.c). */
#include "../../../ble/shim/shim.h"

/* ---- the platform the shim asks for */
uint32_t shim_ms(void) { return fm1_ms; }

static volatile uint32_t shim_depth;
uint32_t shim_irq_save(void)                     /* nests, unlike irq_save(): the shim calls itself */
{
    fm1_irq_off();
    return shim_depth++;
}
void shim_irq_restore(uint32_t f)
{
    (void)f;
    if (shim_depth && --shim_depth == 0)
        fm1_irq_on();
}
int shim_in_irq(void) { return 0; }
/* every task waits: run what is due (a timer may be what they wait for) and keep the watchdog fed,
 * whichever context we are in (btstack_init waits in the main loop's) */
static void ble_stuck_check(void);
void shim_idle(void)
{
    static uint8_t in;
    fm1_wdt_feed();
    if (!in) {
        in = 1;
        shim_timers();
        ble_stuck_check();
        in = 0;
    }
}
void shim_reboot(void) { fm1_reboot(); }
void shim_wdt(void) { fm1_wdt_feed(); }

/* the radio's interrupts sit above the audio render (ALNK0, 3) and the input timer (4): JieLi
 * asks for 0..2; BLE's own timing cannot wait out a render */
static uint8_t ble_irqs[8], ble_nirq;
void shim_request_irq(int index, int prio, void (*handler)(void))
{
    uint32_t f = irq_save();
    fm1_guard_unlock_top();
    fm1_irq_attach((uint32_t)index, handler, (uint32_t)(prio + 5 > 7 ? 7 : prio + 5));
    fm1_guard_lock_top();
    irq_restore(f);
    if (ble_nirq < sizeof ble_irqs)
        ble_irqs[ble_nirq++] = (uint8_t)index;
}

/* a short log for the screen, kept through a reset (.noinit): after a crash or a watchdog reset the
 * Setup page shows what the last run said */
#define BLE_LOG_N 12
#define BLE_LOG_MAGIC 0x424C4531u                 /* "BLE1" */
static struct {
    uint32_t magic, w, count, crash_seen, running, repeat;
    char line[BLE_LOG_N][40];
} blog __attribute__((section(".noinit")));
#define ble_log_w blog.w
#define ble_log_count blog.count
void shim_log(const char *s)
{
    char *d = blog.line[blog.w % BLE_LOG_N];
    uint32_t i;
    if (blog.magic != BLE_LOG_MAGIC)
        return;
    if (blog.w) {                                 /* the same start as the last line: fold it, "(xN)" */
        char *p = blog.line[(blog.w - 1u) % BLE_LOG_N];
        for (i = 0; i < 5u && s[i] && s[i] == p[i]; i++)
            ;
        if (i == 5u || (!s[i] && !p[i])) {
            uint32_t n = ++blog.repeat + 1u, k = 0;
            char num[12];
            for (i = 0; i < 26u && s[i]; i++)
                p[i] = s[i] >= 32 && s[i] < 127 ? s[i] : ' ';
            p[i++] = ' ';
            p[i++] = 'x';
            do {
                num[k++] = (char)('0' + n % 10u);
                n /= 10u;
            } while (n && k < 10u);
            while (k)
                p[i++] = num[--k];
            p[i] = 0;
            blog.count++;
            return;
        }
    }
    blog.repeat = 0;
    for (i = 0; i < 39u && s[i]; i++)
        d[i] = s[i] >= 32 && s[i] < 127 ? s[i] : ' ';
    d[i] = 0;
    blog.w++;
    blog.count++;
}
static void hex8(char *b, uint32_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15u];
}
/* at boot: what ended the last BLE run */
static void ble_boot_note(void)
{
    char b[40] = "crash pc ________ v__ r ________";
    if (blog.magic != BLE_LOG_MAGIC || blog.w > 100000u)
        return;
    if (fm1_crash.magic == FM1_CRASH_MAGIC && fm1_crash.count != blog.crash_seen) {
        blog.crash_seen = fm1_crash.count;
        hex8(b + 9, fm1_crash.pc);
        b[19] = (char)('0' + fm1_crash.vec / 10 % 10);
        b[20] = (char)('0' + fm1_crash.vec % 10);
        hex8(b + 24, fm1_crash.rets);
        shim_log(b);
    } else if (blog.running) {
        shim_log("reset, no crash: watchdog?");
    }
    blog.running = 0;
}

int shim_store_load(void *dst, uint32_t max) { return plat_store_load(OBJ_BLE, dst, max); }
int shim_store_save(const void *src, uint32_t len) { return plat_store_save(OBJ_BLE, src, len); }

/* ---- the stack */
enum { BLE_OFF, BLE_STARTING, BLE_READY, BLE_FAILED };
static volatile uint8_t ble_state;
static volatile uint32_t ble_events;
extern void btstack_init(void);
extern void bt_ble_init(void);
extern void lmp_set_sniff_disable(void);
extern void lib_make_ble_address(uint8_t *ble, uint8_t *edr);
extern int le_controller_set_mac(void *addr);
extern int syscfg_read(uint16_t id, void *buf, uint16_t len);
extern int syscfg_write(uint16_t id, void *buf, uint16_t len);

struct bt_event_s { uint8_t event; uint8_t args[7]; uint32_t value; };
void shim_bt_event(int from, const void *event, int len)
{
    const struct bt_event_s *e = event;
    (void)len;
    ble_events++;
    {
        char b[40] = "event from ";
        b[11] = (char)('0' + (from & 7));
        b[12] = ' ';
        b[13] = 'e';
        b[14] = (char)('0' + (e ? e->event / 10 % 10 : 0));
        b[15] = (char)('0' + (e ? e->event % 10 : 0));
        b[16] = 0;
        shim_log(b);
    }
    if (from == 3 && e && e->event == 3 && ble_state == BLE_STARTING) {   /* BT_EVENT_FROM_CON, BT_STATUS_INIT_OK */
        shim_log("BT init ok: starting BLE");
        bt_ble_init();
        ble_state = BLE_READY;
    }
}

/* the BT address: the one kept, or one made from the chip's unique data (the SDK's own fallback is
 * a random one, kept) */
#define CFG_BT_MAC_ADDR 1                         /* include_lib/utils/syscfg/syscfg_id.h */
const uint8_t *bt_get_mac_addr(void)
{
    static uint8_t mac[6];
    static const uint8_t none[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t i, h = 2166136261u;
    if (syscfg_read(CFG_BT_MAC_ADDR, mac, 6) == 6 && memcmp(mac, none, 6))
        return mac;
    h ^= fm1_ticks();
    for (i = 0; i < 6u; i++) {
        h = (h ^ (h >> 13)) * 16777619u + fm1_ticks();
        mac[i] = (uint8_t)(h >> 8);
    }
    mac[0] = (uint8_t)((mac[0] & 0xFC) | 0xC0);   /* a static random address */
    syscfg_write(CFG_BT_MAC_ADDR, mac, 6);
    return mac;
}

static uint32_t ble_t0;
extern void shim_os_dump(void);
static void ble_stuck_check(void)                 /* still starting after 3 s: say what each task waits for */
{
    static uint32_t said;
    if (ble_state == BLE_STARTING && fm1_ms - ble_t0 > 3000u && said != ble_t0) {
        said = ble_t0;
        shim_log("still starting after 3 s:");
        shim_os_dump();
    }
}

static void ble_start(void)
{
    uint8_t addr[6];
    if (ble_state != BLE_OFF)
        return;
    ble_state = BLE_STARTING;
    if (fm1_crash.magic != FM1_CRASH_MAGIC)
        fm1_crash.count = 0;
    blog.magic = BLE_LOG_MAGIC;
    blog.w = blog.count = 0;
    blog.crash_seen = fm1_crash.count;
    blog.running = 1;
    ble_t0 = fm1_ms;
    shim_log("BT: starting");
    shim_os_init();
    fm1_guard_stack_floor(shim_stacks_lo());      /* the tasks' stacks are in the pool */
    lmp_set_sniff_disable();
    lib_make_ble_address(addr, (uint8_t *)bt_get_mac_addr());
    le_controller_set_mac(addr);
    btstack_init();
}

/* the main loop's turn: timers, then every BT task that can run */
static void ble_poll(void)
{
    if (ble_state == BLE_OFF)
        return;
    shim_timers();
    shim_run();
    ble_stuck_check();
}

static int ble_status(void) { return ble_state; }
static const char *ble_log_line(int back)
{
    if ((uint32_t)back >= ble_log_count || back >= BLE_LOG_N)
        return "";
    return blog.line[(blog.w - 1u - (uint32_t)back) % BLE_LOG_N];
}
static uint32_t ble_log_total(void) { return blog.magic == BLE_LOG_MAGIC ? blog.count : 0u; }
