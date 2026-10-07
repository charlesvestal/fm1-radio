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
void shim_idle(void) {}
void shim_reboot(void) { fm1_reboot(); }

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

/* a short log for the screen */
#define BLE_LOG_N 6
static char ble_log[BLE_LOG_N][40];
static uint32_t ble_log_w, ble_log_count;
void shim_log(const char *s)
{
    char *d = ble_log[ble_log_w % BLE_LOG_N];
    uint32_t i;
    for (i = 0; i < 39u && s[i]; i++)
        d[i] = s[i] >= 32 && s[i] < 127 ? s[i] : ' ';
    d[i] = 0;
    ble_log_w++;
    ble_log_count++;
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

static void ble_start(void)
{
    uint8_t addr[6];
    if (ble_state != BLE_OFF)
        return;
    ble_state = BLE_STARTING;
    shim_log("BT: starting");
    shim_os_init();
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
}

static int ble_status(void) { return ble_state; }
static const char *ble_log_line(int back)
{
    if ((uint32_t)back >= ble_log_count || back >= BLE_LOG_N)
        return "";
    return ble_log[(ble_log_w - 1u - (uint32_t)back) % BLE_LOG_N];
}
static uint32_t ble_log_total(void) { return ble_log_count; }
