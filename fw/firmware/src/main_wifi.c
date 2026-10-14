/* fm1-radio: an internet radio for the FM-1, on the platform layer (boot, LCD, USB, panel), JieLi's WiFi
 * libraries on the cooperative shim (ble/shim, which started as fm1-ble's for the BT libraries), lwIP,
 * BearSSL and minimp3.
 * Build: tools/build.py --wifi */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"
#include "fm1_flash.h"
#include "fm1_xip.h"
#include "fm1_perf.h"

#define SHIM_WATCH_ADDR 1                /* the trim crash: see ble.c shim_delay_us */
#define SHIM_WATCH_FRAME 32u             /* shim_delay_us's own frame (its prologue, from the disassembly) */
#define CV_MAX (240u * 48u)           /* rows of at most 48: the rest of the pool goes to the WiFi heap */
#include "libc.c"
#include "lcd.c"
#include "gfx.c"

static volatile uint32_t fm1_ms;
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

#include "app/plat.h"

#define FELUCCA_OTA 1
#ifndef OM_CDC
#define OM_CDC 0
#endif
#define FELUCCA_CDC OM_CDC
#define FELUCCA_UAC 0
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_x00"
#endif
#include "usb.c"
#include "midi_uart.c"

static uint8_t flash_ok;
static void audio_silence(void) {}
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc) return rc;
        off += k; d += k; n -= k;
    }
    return 0;
}
static int st_erase(uint32_t off)
{
    uint32_t took;
    if (!FL_STORE_OK(off, 0x1000u)) return -8;
    return fl_erase4k(off, &took);
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    if (!FL_STORE_OK(off, n)) return -8;
    return fl_write(off, src, n);
}
#include "storage.c"

struct bootguard_s { uint32_t magic, failed, pending; };
extern struct bootguard_s bootguard;
#include "ota.c"
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu)) return -8;
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n)) return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
    if (step < 9u)
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_HI, 1);
    else {
        draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_HI, 1);
        fm1_delay_ms(1500);
    }
}
static void ota_commit(const uint8_t *parm)
{
    bootguard.pending = 0;
    bootguard.failed = 0;
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}

/* ---- panel (button / encoder map) ---- */
#include "app/panel.c"

/* ---- platform (plat.h on Felucca HAL) ---- */
#define MQ 64u
static uint32_t midi_in_q[MQ];
static volatile uint32_t mi_r, mi_w;
uint32_t plat_ms(void) { return fm1_ms; }
uint32_t plat_buttons(void)
{
    uint32_t raw = fm1_in.buttons, out = 0, i;
    for (i = 0; i < NB; i++)
        if (raw & (1u << panel.btn[i]))
            out |= 1u << i;
    return out;
}
uint32_t plat_keys(void) { return fm1_in.notes & ((1u << NKEYS) - 1u); }
int32_t plat_enc(int role)
{
    if (role < 0 || role >= NE) return 0;
    return fm1_enc_take(panel.enc[role]) * panel.dir[role];
}
static volatile uint32_t master_q12 = 2048;
uint32_t plat_master(void) { return master_q12; }
static uint8_t led_pos[FM1_NKEY];
static void led_pos_init(void)
{
    uint32_t id, p, r;
    for (id = 0; id < FM1_NKEY; id++) {
        led_pos[id] = 0xFF;
        for (p = 0; p < FM1_NCOL; p++)
            for (r = 1; r < 5u; r++)
                if (FM1_KEYMAP[r][p] == (int8_t)id)
                    led_pos[id] = (uint8_t)((p << 3) | r);
    }
}
void plat_leds(uint32_t buttons, uint32_t keys)
{
    uint8_t nl[FM1_NCOL] = {0};
    uint32_t i, c;
    for (i = 0; i < NB + NKEYS; i++) {
        uint32_t id = i < NB ? panel.btn[i] : 14u + (i - NB);
        int on = i < NB ? (buttons >> i) & 1u : (keys >> (i - NB)) & 1u;
        uint8_t q = led_pos[id];
        if (on && q != 0xFF)
            nl[q >> 3] |= (uint8_t)(1u << (q & 7u));
    }
    for (c = 0; c < FM1_NCOL; c++)
        fm1_led[c] = nl[c];
}
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w) return 0;
    *pkt = midi_in_q[mi_r % MQ];
    mi_r++;
    return 1;
}
void plat_midi_out(uint32_t pkt) { midi_out_event(pkt); }
int plat_store_load(uint32_t obj, void *dst, uint32_t max) { return flash_ok ? st_load(obj, dst, max) : -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { return flash_ok ? st_save(obj, src, len) : -9; }
static volatile uint32_t audio_cpu_pct, audio_xruns;
uint32_t plat_cpu_pct(void) { return audio_cpu_pct; }
uint32_t plat_xruns(void) { return audio_xruns; }

/* ---- stubs for DSP symbols that ble.c references ---- */
volatile uint32_t om_fault[5], om_fault_count;

/* ---- freezes: the watchdog is fed only while the timer interrupt still ticks (wdt_feed_alive); a second
 * without it, a note is left (.noinit) and the watchdog resets. The audio interrupt is watched too. */
static volatile uint32_t hb_tick, hb_audio, icfg_in_isr;
static void wdt_feed_alive(void);
#define SHIM_WDT_FEED() wdt_feed_alive()
#define FREEZE_MAGIC 0x46525A31u                  /* "FRZ1" */
static struct {
    uint32_t magic, ms, depth, pcm_r, pcm_w, radio, wifi, audio;
    char task[16];
} freeze_note __attribute__((section(".noinit")));
static volatile uint8_t stall_flag;
static uint32_t stall_pnd[8], stall_ms;
static const char *stall_task;

/* ---- the shim's platform side (app/ble.c, fm1-ble's, without its BLE stack) ---- */
#define SHIM_NO_BLE 1
#define OBJ_BLE 2
#define BLE_LOG_N 64                      /* 2.5 KB: NOINIT's room goes to the https client's state */
#define OBJ_WIFI OBJ_PROJ                 /* no project in this firmware: its object holds the networks */
#include "app/ble.c"

/* ---- WiFi ---- */
#include "app/wifi.c"
#include "app/radio.c"

/* ---- audio: silent (no synth in this PoC) ---- */
#define HALF_FRAMES 256u
#define HALF_WORDS (HALF_FRAMES * 2u)
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

void fm1_alnk0_irq(void)
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        hb_audio++;
        uint32_t half = fm1_audio_free_half(), k;
        int32_t *o = &abuf[half * HALF_WORDS];
        (void)k;
        radio_render(o, HALF_FRAMES);
        fm1_audio_ack_half();
    }
}
extern void isr_alnk0(void);

static void audio_init(void)
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}

/* ---- timer ISR ---- */
extern volatile unsigned long jiffies;
static uint8_t usb_due;
void fm1_timer5_irq(void)
{
    static uint32_t sub;
    fm1_timer5_ack();
    fm1_input_tick();
    hb_tick++;
    icfg_in_isr = fm1_icfg();                     /* the probe: the enable state inside an interrupt */
    {   /* the audio interrupt stopped while the radio plays: note the moment (the main loop logs it) */
        static uint32_t a_last, a_ms;
        if (hb_audio != a_last) {
            a_last = hb_audio;
            a_ms = fm1_ms;
        } else if (radio_st == RS_PLAYING && fm1_ms - a_ms > 300u && !stall_flag) {
            uint32_t k;
            for (k = 0; k < 8u; k++)
                stall_pnd[k] = 0;
            fm1_ipnd_or(stall_pnd);
            stall_task = os_current_task();
            stall_ms = fm1_ms;
            stall_flag = 1;
        }
    }
    if (sub % 5u == 0u)
        usb_due = 1;
    if (usb_due) {
        usb_due = 0;
        usb_poll();
        uart_midi_poll();
    }
    if (++sub == 10u) sub = 0;
    {
        static uint32_t last, acc;
        uint32_t now = fm1_ticks();
        acc += now - last;
        last = now;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
        jiffies = shim_ms() / 10u;               /* the WiFi driver's timers (10 ms ticks) */
    }
}
extern void isr_timer5(void);

/* ---- boot ---- */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];

#define BOOTGUARD_MAGIC 0x42475244u
struct bootguard_s bootguard __attribute__((section(".noinit")));

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

extern const char *os_current_task(void);
/* the task running at the fault, kept through the reset for the wire dump */
static char crash_task[16] __attribute__((section(".noinit")));

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    {
        const char *t = os_current_task();
        uint32_t i;
        for (i = 0; i < sizeof crash_task - 1u && t && t[i]; i++)
            crash_task[i] = t[i];
        crash_task[i] = 0;
    }
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 8, 240, &FONT_S, "WiFi PoC CRASH", C_WHITE, 1);
    hexs(b, c->vec);
    draw_text_box(10, 40, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->pc);
    draw_text_box(10, 60, 220, &FONT_S, b, C_WHITE, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void enter_uboot(const char *why)
{
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_S, why, C_WHITE, 1);
    fm1_delay_ms(20);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;
    fm1_enter_uboot();
}

/* fed while the 1 ms timer interrupt ticks; a second without it (interrupts off somewhere), a note, then the
 * watchdog's reset */
static void wdt_feed_alive(void)
{
    static uint32_t seen, seen_ms;
    uint32_t now = shim_ms();                     /* TIMER4: runs with the interrupts off */
    if (hb_tick != seen || irqs_masked) {         /* (BLE's start masks the timer on purpose) */
        seen = hb_tick;
        seen_ms = now;
    } else if (now - seen_ms > 1000u) {
        if (freeze_note.magic != FREEZE_MAGIC || freeze_note.ms != seen_ms) {
            const char *t = os_current_task();
            uint32_t i;
            freeze_note.magic = FREEZE_MAGIC;
            freeze_note.ms = seen_ms;
            freeze_note.depth = shim_depth;
            freeze_note.pcm_r = pcm_r;
            freeze_note.pcm_w = pcm_w;
            freeze_note.radio = radio_st;
            freeze_note.wifi = wifi_st;
            freeze_note.audio = hb_audio;
            for (i = 0; i < sizeof freeze_note.task - 1u && t && t[i]; i++)
                freeze_note.task[i] = t[i];
            freeze_note.task[i] = 0;
        }
        return;                                   /* not fed: the watchdog resets */
    }
    fm1_wdt_feed();
}

/* ---- the wire: SysEx F0 7D 57 <cmd> [data] F7 (tools/wifi_ctl.py, web/wifi.html). Text bytes travel as
 * two nibbles each (an SSID may hold any byte).
 *   1 WiFi on, 5 WiFi off, 3 scan, 4 scan into the log, 0x10 dump (lines F0 7D 57 10 <text> F7,
 *   then F0 7D 57 11 F7), 0x12 forget the crash, 0x20 add <ssid 0 pass>, 0x21 list (F0 7D 57 21 <i>
 *   <ssid> F7 each, then 11), 0x22 delete <i>, 0x23 join saved <i>, 0x24 status (F0 7D 57 24 <text> F7),
 *   0x7F reboot. Actions run in the main loop; the dump, the list and the status are answered at once,
 *   also while the main loop waits in the library (shim_idle_hook). */
extern int shim_timers_used(void);
static volatile uint8_t wire_cmd, wire_arg, wire_arg2;
static uint8_t wire_buf[5];                       /* a command's first argument bytes */
static uint32_t main_loops;
static char wire_ssid[33], wire_pass[64];

static void wire_send(uint8_t code, int idx, const char *s, int nibbles)
{
    uint8_t m[160];
    uint32_t n = 0;
    m[n++] = 0xF0; m[n++] = 0x7D; m[n++] = 0x57; m[n++] = code;
    if (idx >= 0)
        m[n++] = (uint8_t)idx;
    while (s && *s && n < sizeof m - 3u) {
        uint8_t b = (uint8_t)*s++;
        if (nibbles) {
            m[n++] = b >> 4;
            m[n++] = b & 15u;
        } else {
            m[n++] = b & 0x7F;
        }
    }
    m[n++] = 0xF7;
    ota_wire_send(m, n);
}
static void wire_line(const char *s) { wire_send(0x10, -1, s, 0); }
static void wire_end(void) { wire_send(0x11, -1, 0, 0); }

static void radio_ui_dump(void);                  /* radio_ui.c */
static void wire_dump(void)
{
    char b[72];
    int i;
    snprintf(b, sizeof b, "up %u ms  wifi %s  isr %u  tmr %d", fm1_ms, wifi_status_str(),
             shim_isr_trace.n, shim_timers_used());
    wire_line(b);
    radio_ui_dump();
    {
        extern uint32_t shim_heap_used, shim_heap_peak;
        extern const uint32_t shim_heap_size;
        snprintf(b, sizeof b, "heap %u used, %u peak, of %u  isr-blocks %u", shim_heap_used, shim_heap_peak,
                 shim_heap_size, shim_isr_blocks);
        wire_line(b);
    }
    snprintf(b, sizeof b, "radio %s  net %d  buf %u/%u  pcm %u  fr %u  %uHz %ukbps", radio_status_str(),
             radio_net_state, radio_wr - radio_rd, NET_RING, pcm_w - pcm_r, radio_frames, radio_hz, radio_kbps);
    wire_line(b);
    snprintf(b, sizeof b, "  under %u  rebuf %u  reconn %u  %s", radio_underruns, radio_rebuffers, radio_reconnects,
             radio_why);
    wire_line(b);
    {   /* rates since the last dump: what came in, what was decoded, how often the main loop went round */
        static uint32_t t_last, wr_last, fr_last, loops_last;
        uint32_t dt = fm1_ms - t_last ? fm1_ms - t_last : 1u;
        snprintf(b, sizeof b, "  mode %u  in %u kbit/s  decoded %u fr/s (need 38)  %u us/fr  loop %u/s  rebuf %u",
                 radio_mode, (radio_wr - wr_last) * 8u / dt, (radio_frames - fr_last) * 1000u / dt,
                 radio_dec_n ? radio_dec_us / radio_dec_n : 0u, (main_loops - loops_last) * 1000u / dt,
                 radio_rebuffers);
        wire_line(b);
        t_last = fm1_ms;
        wr_last = radio_wr;
        fr_last = radio_frames;
        loops_last = main_loops;
    }
    {
        uint32_t on = fm1_icfg(), off;
        uint32_t f = irq_save();
        off = fm1_icfg();
        irq_restore(f);
        snprintf(b, sizeof b, "icfg: main %08x  cli %08x  in timer isr %08x", on, off, icfg_in_isr);
        wire_line(b);
        snprintf(b, sizeof b, "wifi isr %08x", shim_isr_trace.rsv[0]);
        wire_line(b);
    }
    {   /* clocks (SYS_DIV, CLK_CON0, CLK_CON1, SFC_CON) and the P33 analog regulators (ANA_CON0..9) */
        uint32_t c[4], k;
        uint8_t an[10];
        fm1_clk_snapshot(c);
        for (k = 0; k < 10u; k++)
            an[k] = fm1_p33_read(k);
        snprintf(b, sizeof b, "clk %08x %08x %08x %08x", c[0], c[1], c[2], c[3]);
        wire_line(b);
        snprintf(b, sizeof b, "ana %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", an[0], an[1], an[2], an[3],
                 an[4], an[5], an[6], an[7], an[8], an[9]);
        wire_line(b);
    }
    snprintf(b, sizeof b, "reset: p3 %02x src %08x wdt %02x", fm1_boot.p3_rst, fm1_boot.rst_src, fm1_boot.wdt_con);
    wire_line(b);
    if (freeze_note.magic == FREEZE_MAGIC) {
        snprintf(b, sizeof b, "freeze at %u ms: task %s depth %u radio %u wifi %u pcm %u/%u audio %u", freeze_note.ms,
                 freeze_note.task, freeze_note.depth, freeze_note.radio, freeze_note.wifi, freeze_note.pcm_r,
                 freeze_note.pcm_w, freeze_note.audio);
        wire_line(b);
    }
    if (fm1_crash.magic == FM1_CRASH_MAGIC) {
        snprintf(b, sizeof b, "crash n=%u vec=%u pc=%08x rets=%08x up=%u", fm1_crash.count, fm1_crash.vec,
                 fm1_crash.pc, fm1_crash.rets, fm1_crash.uptime_ms);
        wire_line(b);
        snprintf(b, sizeof b, "  sp=%08x usp=%08x ssp=%08x psr=%08x task=%s", fm1_crash.sp, fm1_crash.usp,
                 fm1_crash.ssp, fm1_crash.psr, crash_task);
        wire_line(b);
        snprintf(b, sizeof b, "  emu=%08x dbg=%08x etm=%08x %08x %08x %08x", fm1_crash.emu, fm1_crash.dbg,
                 fm1_crash.etm[0], fm1_crash.etm[1], fm1_crash.etm[2], fm1_crash.etm[3]);
        wire_line(b);
    } else {
        wire_line("no crash recorded");
    }
    for (i = (int)(ble_log_total() < BLE_LOG_N ? ble_log_total() : BLE_LOG_N) - 1; i >= 0; i--)
        wire_line(ble_log_line(i));
    wire_end();
}

static void wire_status(void)
{
    char b[96];
    snprintf(b, sizeof b, "%s|%s|%s|%d|%s", wifi_status_str(), wifi_ssid, wifi_ip_str,
             wifi_st == WS_CONNECTED ? wifi_rssi : 0, wifi_why);
    wire_send(0x24, -1, b, 1);
}

static const uint8_t WHITE_KEY[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* F3 .. G5 */
static uint32_t fx_wire;
static volatile uint8_t url_restart;
#include "app/rg.c"

static void wire_service(void)
{
    const uint8_t *p;
    uint32_t n, i;
    uint8_t cmd;
    if (!ota_frame_get(&p, &n) || n < 3u || p[0] != 0x7D || p[1] != 0x57)
        return;
    cmd = p[2];
    if (cmd == 0x51) {                            /* the station table's next chunk (rg.c) */
        int taken = rg_take_chunk(p + 3, n - 3u);
        ota_frame_done();
        if (taken)
            rg_reply("next", rgu.got);            /* busy or not begun: the host resends from here */
        return;
    }
    if (cmd == 0x43) {                            /* the stream's url, as nibbles */
        uint32_t k = 0;
        for (i = 3; i + 1u < n && k + 1u < sizeof radio_url_buf; i += 2)
            radio_url_buf[k++] = (char)((p[i] & 15u) << 4 | (p[i + 1] & 15u));
        radio_url_buf[k] = 0;
        radio_url = k ? radio_url_buf : RADIO_URL;
        url_restart = 1;                          /* the main loop restarts the stream on it */
    }
    if (cmd == 0x20) {                            /* ssid 0 pass, as nibbles */
        char *d = wire_ssid;
        uint32_t k = 0, max = sizeof wire_ssid;
        wire_ssid[0] = wire_pass[0] = 0;
        for (i = 3; i + 1u < n; i += 2) {
            char c = (char)((p[i] & 15u) << 4 | (p[i + 1] & 15u));
            if (!c) {
                d[k] = 0;
                if (d == wire_pass)
                    break;
                d = wire_pass, k = 0, max = sizeof wire_pass;
                continue;
            }
            if (k + 1u < max)
                d[k++] = c;
            d[k] = 0;
        }
    }
    wire_arg = n > 3u ? p[3] : 0;
    wire_arg2 = n > 4u ? p[4] : 0;
    for (i = 0; i < 5u; i++)
        wire_buf[i] = n > 3u + i ? p[3 + i] : 0;
    ota_frame_done();
    if (cmd == 0x10) {
        shim_os_dump();
        wire_dump();
    } else if (cmd == 0x21) {
        for (i = 0; i < NET_N && nets.n[i].ssid[0]; i++)
            wire_send(0x21, (int)i, nets.n[i].ssid, 1);
        wire_end();
    } else if (cmd == 0x24) {
        wire_status();
    } else if (cmd == 0x31) {                     /* crystal caps for the next start (0 0: the default) */
        extern u8 shim_xosc[2];
        shim_xosc[0] = wire_arg;
        shim_xosc[1] = wire_arg2;
        wire_line("xosc set");
    } else if (cmd == 0x32) {                     /* the reception score: read, then reset */
        char b[72];
        uint32_t fn = fr_n;
        snprintf(b, sizeof b, "scans %u hits %u avg %d  frames %u foff %d rssi %d", meas_scans, meas_hits,
                 meas_hits ? (int)(meas_rssi / (int32_t)meas_hits) : 0, fn,
                 fn ? (int)(fr_foff / (int32_t)fn) : 0, fn ? (int)(fr_rssi / (int32_t)fn) : 0);
        meas_scans = meas_hits = 0;
        meas_rssi = 0;
        fr_n = 0;
        fr_foff = fr_rssi = 0;
        wire_line(b);
    } else if (cmd == 0x43) {
        wire_line(radio_url);
    } else if (cmd == 0x12) {
        fm1_crash.magic = 0;
        freeze_note.magic = 0;
        wire_line("crash record cleared");
    } else if (cmd == 0x7F) {
        wire_line("rebooting");
        fm1_delay_ms(50);
        usb_detach();
        fm1_delay_ms(30);
        bootguard.pending = 0;
        fm1_reboot();
    } else {
        wire_cmd = cmd;                           /* the main loop's */
    }
}

/* ---- the screen: FoMni's paper look; a row is drawn only when what it shows changes */
#define K_BG RGB(250, 244, 232)
#define K_PANEL RGB(236, 228, 212)
#define K_LINE RGB(216, 205, 186)
#define K_DIM RGB(150, 136, 118)
#define K_TEXT RGB(72, 58, 48)
#define K_CORAL RGB(242, 120, 72)
#define K_TEAL RGB(38, 166, 154)

enum { PG_HOME, PG_BROWSE, PG_WIFI, PG_NETS, PG_PASS, PG_HELP };
static uint8_t ui_page, ui_dirty = 1, ui_leds_off;   /* ui_leds_off: the wire's test switch (0x30) */
static int ui_sel, ui_top;
static uint32_t ui_scan_t;                        /* the scan asked for (ms), 0 = none waiting */
static char ui_msg[40];
#define UI_ROWS 14
static char ui_key[UI_ROWS][80] OM_POOL;

static void ui_row(int slot, uint32_t y, uint32_t h, const char *l, const char *r, uint16_t lc, uint16_t rc,
                   uint16_t bg)
{
    char key[80];
    int32_t rw;
    snprintf(key, sizeof key, "%u%s|%s|%x%x%x", y, l, r, lc, rc, bg);
    if (slot < UI_ROWS) {
        if (w_eq(ui_key[slot], key))
            return;
        memcpy(ui_key[slot], key, sizeof key);
    }
    cv_begin(240, h, bg);
    cv_text(16, (int32_t)(h - FONT_S.h) / 2, &FONT_S, l, lc);
    rw = text_w(&FONT_S, r);
    cv_text(224 - rw, (int32_t)(h - FONT_S.h) / 2, &FONT_S, r, rc);
    cv_blit(0, y);
}
static void ui_title(int slot, const char *t, const char *hint)
{
    char key[80];
    snprintf(key, sizeof key, "T%s|%s", t, hint);
    if (w_eq(ui_key[slot], key))
        return;
    memcpy(ui_key[slot], key, sizeof key);
    cv_begin(240, 36, K_BG);
    cv_text(16, 10, &FONT_B, t, K_TEXT);
    cv_text(224 - text_w(&FONT_S, hint), 10, &FONT_S, hint, K_DIM);
    cv_rect(16, 34, 208, 1, K_LINE);
    cv_blit(0, 0);
}

static void ui_status(void)
{
    char ssid[33], v[24];
    uint16_t wc = wifi_st == WS_CONNECTED ? K_TEAL : wifi_st == WS_FAILED ? K_CORAL : K_TEXT;
    ui_title(0, "WiFi", "");
    ui_row(1, 48, 26, "WiFi", wifi_status_str(), K_DIM, wc, K_BG);
    ssid_show(ssid, wifi_ssid[0] ? wifi_ssid : "-", sizeof ssid);
    if (wifi_st == WS_OFF)
        ssid[0] = 0;
    ui_row(2, 74, 26, "Network", ssid, K_DIM, K_TEXT, K_BG);
    ui_row(3, 100, 26, "Address", wifi_st == WS_FAILED ? wifi_why : wifi_ip_str, K_DIM,
           wifi_st == WS_FAILED ? K_CORAL : K_TEXT, K_BG);
    v[0] = 0;
    if (wifi_st == WS_CONNECTED)
        snprintf(v, sizeof v, "%d dBm", wifi_rssi);
    ui_row(4, 126, 26, "Signal", v, K_DIM, K_TEXT, K_BG);
    {
        char nm[40];
        uint16_t rc = radio_st == RS_PLAYING ? K_TEAL : radio_st == RS_ERROR ? K_CORAL : K_TEXT;
        if (radio_st == RS_PLAYING && radio_net_name[0])
            ssid_show(nm, radio_net_name, 22);
        else if (radio_st == RS_ERROR)
            w_cpy(nm, radio_why, sizeof nm);
        else
            w_cpy(nm, radio_status_str(), sizeof nm);
        ui_row(5, 152, 26, "Radio", nm, K_DIM, rc, K_BG);
    }
    ui_row(13, 214, 26, wifi_st == WS_OFF ? "PLAY wifi on  SEL networks" : "PLAY wifi off  SEL networks",
           "HOME", K_DIM, K_DIM, K_PANEL);
}

static void ui_nets(void)
{
    int i;
    ui_title(0, "Networks", "SEL  scan again");
    if (scan_n < 0) {
        ui_row(1, 40, 20, ui_scan_t ? "Scanning..." : "", "", K_DIM, K_DIM, K_BG);
        for (i = 1; i < 7; i++)
            ui_row(1 + i, 40 + 20u * (uint32_t)i, 20, "", "", K_DIM, K_DIM, K_BG);
    } else {
        if (ui_sel >= scan_n)
            ui_sel = scan_n ? scan_n - 1 : 0;
        if (ui_sel < ui_top)
            ui_top = ui_sel;
        if (ui_sel >= ui_top + 7)
            ui_top = ui_sel - 6;
        for (i = 0; i < 7; i++) {
            int k = ui_top + i;
            char l[40], r[24], ss[33];
            uint16_t bg = K_BG, fg = K_TEXT;
            l[0] = r[0] = 0;
            if (k < scan_n) {
                scan_t *s = &scan_list[k];
                ssid_show(ss, s->ssid, 17);
                snprintf(l, sizeof l, "%s%s", s->saved ? "* " : "", ss);
                snprintf(r, sizeof r, "%d %s", s->rssi, s->secured ? "lock" : "open");
                if (w_eq(s->ssid, wifi_ssid) && wifi_st == WS_CONNECTED)
                    fg = K_TEAL;
                if (k == ui_sel)
                    bg = K_PANEL;
            } else if (k == 0) {
                snprintf(l, sizeof l, "No networks found");
                fg = K_DIM;
            }
            ui_row(1 + i, 40 + 20u * (uint32_t)i, 20, l, r, fg, k == ui_sel ? K_CORAL : K_DIM, bg);
        }
    }
    ui_row(12, 184, 28, ui_msg, "", K_CORAL, K_DIM, K_BG);
    ui_row(13, 214, 26, "PLAY join  SAVE password", "HOME back", K_DIM, K_DIM, K_PANEL);
}

/* ---- the password page: the 27 keys type (F3 = a ... E5 = z, G5 = delete); hold OCT- for capitals,
 * OCT+ for digits (F3 .. D4) and space / symbols (D#4 .. F#5) */
static char pw_ssid[33], pw_buf[64];
static int pw_len;
static uint32_t pw_keys;                          /* held, for the drawing */
static const char PW_SYM[] = " !@#$%&*-_.,?+/:";

static int pw_layer(uint32_t b)
{
    return (b & (1u << B_OCTUP)) ? 2 : (b & (1u << B_OCTDN)) ? 1 : 0;
}
static char pw_char(int k, int layer)
{
    if (k >= 26)
        return 0;                                 /* delete */
    if (layer == 2)
        return k < 10 ? (char)('0' + k) : PW_SYM[k - 10];
    return (char)((layer == 1 ? 'A' : 'a') + k);
}
static void pw_open(const char *ssid)
{
    int i = nets_find(ssid);
    w_cpy(pw_ssid, ssid, sizeof pw_ssid);
    pw_buf[0] = 0;
    pw_len = 0;
    if (i >= 0) {
        w_cpy(pw_buf, nets.n[i].pass, sizeof pw_buf);
        while (pw_buf[pw_len])
            pw_len++;
    }
    ui_page = PG_PASS;
    ui_dirty = 1;
    ui_msg[0] = 0;
}
static void pw_key(int k, int layer)
{
    char c = pw_char(k, layer);
    if (!c) {
        if (pw_len)
            pw_buf[--pw_len] = 0;
    } else if (pw_len < (int)sizeof pw_buf - 1) {
        pw_buf[pw_len++] = c;
        pw_buf[pw_len] = 0;
    }
}

/* F3 .. G5 as a little piano: 16 white keys, 11 black, each with what it types now */
static void ui_piano(int slot, uint32_t y, int layer, uint32_t held)
{
    static const uint8_t BLACK[12] = {0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0};   /* from F */
    char key[80];
    int k, wi = 0;
    const int kw = 14, x0 = 8, wh = 44, bh = 26;
    snprintf(key, sizeof key, "P%d%x", layer, held);
    if (w_eq(ui_key[slot], key))
        return;
    memcpy(ui_key[slot], key, sizeof key);
    cv_begin(240, (uint32_t)wh + 2u, K_BG);
    for (k = 0; k < 27; k++)                      /* white keys first, then the black ones over them */
        if (!BLACK[k % 12]) {
            char t[3] = {pw_char(k, layer), 0, 0};
            int x = x0 + wi * kw;
            if (!t[0])
                t[0] = '<';
            cv_rect(x, 0, kw - 1, wh, (held >> k & 1u) ? K_CORAL : RGB(255, 255, 255));
            cv_rect(x + kw - 1, 0, 1, wh, K_LINE);
            cv_text(x + (kw - 1 - text_w(&FONT_XS, t)) / 2, wh - 16, &FONT_XS, t,
                    (held >> k & 1u) ? RGB(255, 255, 255) : K_TEXT);
            wi++;
        }
    wi = 0;
    for (k = 0; k < 27; k++) {
        if (!BLACK[k % 12]) {
            wi++;
            continue;
        }
        {
            char t[2] = {pw_char(k, layer), 0};
            int x = x0 + wi * kw - 6;
            cv_rect(x, 0, 12, bh, (held >> k & 1u) ? K_CORAL : K_TEXT);
            cv_text(x + (12 - text_w(&FONT_XS, t)) / 2, 6, &FONT_XS, t, RGB(255, 255, 255));
        }
    }
    cv_blit(0, y);
}

static void ui_pass(void)
{
    char ss[33], shown[24];
    int layer = pw_layer(plat_buttons());
    ssid_show(ss, pw_ssid, 22);
    ui_title(0, "Password", "");
    ui_row(1, 40, 22, ss, "", K_DIM, K_DIM, K_BG);
    /* the end of what is typed, with a cursor */
    {
        int from = pw_len > 21 ? pw_len - 21 : 0;
        snprintf(shown, sizeof shown, "%s_", pw_buf + from);
    }
    ui_row(2, 66, 30, shown, "", K_TEXT, K_DIM, K_PANEL);
    ui_row(3, 100, 20, layer == 1 ? "CAPITALS" : layer == 2 ? "DIGITS + SYMBOLS" : "letters",
           "OCT- caps  OCT+ 123", layer ? K_CORAL : K_DIM, K_DIM, K_BG);
    ui_piano(4, 128, layer, pw_keys);
    ui_row(12, 184, 28, ui_msg, "", K_CORAL, K_DIM, K_BG);
    ui_row(13, 214, 26, "PLAY  save + join", "HOME  cancel", K_DIM, K_DIM, K_PANEL);
}

#include "app/radio_ui.c"

static void ui_draw(void)
{
    if (ui_dirty) {
        int i;
        ui_dirty = 0;
        lcd_fill(0, 0, 240, 240, K_BG);
        for (i = 0; i < UI_ROWS; i++)
            ui_key[i][0] = 0;
    }
    switch (ui_page) {
    case PG_HOME: ui_home(); break;
    case PG_BROWSE: ui_browse(); break;
    case PG_WIFI: ui_status(); break;
    case PG_NETS: ui_nets(); break;
    case PG_PASS: ui_pass(); break;
    default: ui_help(); break;
    }
}

static void ui_page_set(int pg)
{
    ui_page = (uint8_t)pg;
    ui_dirty = 1;
    ui_msg[0] = 0;
}

static void ui_scan(void)
{
    scan_n = -1;
    ui_msg[0] = 0;
    if (wifi_st == WS_OFF)
        wifi_power(1);                            /* the scan asks for the radio */
    ui_scan_t = fm1_ms ? fm1_ms : 1;
}

/* ---- main ---- */
static void fm1_main(void)
{
    uint32_t last_frame = 0, prev = 0;
    {
        uint32_t f = irq_save();
        flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
        irq_restore(f);
        if (flash_ok)
            ota_boot_cleanup();
    }
    lcd_init();
    palette_set(0);
    lcd_fill(0, 0, 240, 240, K_BG);
    draw_text_box(0, 112, 240, &FONT_B, "FM-1 Net", K_TEXT, 1);
    lcd_sync();

    fm1_input_init();
    fm1_adc_init();
    panel_init();
    led_pos_init();
    audio_init();
    usb_start();
    uart_midi_init();
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(400);

    /* VDC14 (the RF / analog rail, P33 ANA_CON6[2:0]) to 1.50 V. The bootloader leaves 1.40 V; JieLi's SDK runs
     * 1.45 V with WiFi at this clock (apps/common/system/system_vdd_clock.c). Under WiFi's load at 1.40 V the code
     * fetched from the flash comes back corrupted now and then (jumps to 0x70 / 0x470): measured on a stream, 1.40 V
     * crashes within seconds to minutes, 1.45 V within minutes, 1.50 V ran 16 minutes clean */
    fm1_p33_write(6, (uint8_t)((fm1_p33_read(6) & ~7u) | 5u));
    fl_plain_window_init();                       /* the data flash reads in the clear (the station table) */
    rg_open();
    fx_reset();
    ble_boot_note();
    nets_load();
    if (rg) {                                     /* the station of the last session, playing if it was */
        sel_from_station(nets.last);
        tuned_station = 0xFFFFu;
        tune_want = 1;
        tune_at = 0;
        radio_want = nets.playing;
    }
    if (nets_count())                             /* a radio: WiFi on by itself when it knows a network */
        wifi_power(1);
    shim_idle_hook = wire_service;
    ui_dirty = 1;

    for (;;) {
        uint32_t now, down;
        int32_t turn;
        main_loops++;
        wdt_feed_alive();
        if (stall_flag == 1) {                    /* the audio interrupt stopped: what was pending */
            char lb[40];
            stall_flag = 2;
            snprintf(lb, sizeof lb, "audio stall %u ms %s", stall_ms, stall_task ? stall_task : "?");
            shim_log(lb);
            snprintf(lb, sizeof lb, "pnd %08x %08x %08x", stall_pnd[0], stall_pnd[1], stall_pnd[3]);
            shim_log(lb);
        }
        usb_retry(fm1_ms);

        if (fm1_ms > 30000u && bootguard.pending) {
            bootguard.pending = 0;
            bootguard.failed = 0;
        }

        {   /* MASTER: the volume (FoMni's curve) */
            static int32_t knob = 1024 * 16;
            int32_t a = fm1_adc_read(FM1_ADC_MASTER);
            if (a >= 0) {
                uint32_t k10;
                knob += (a * 16 - knob) / 8;
                k10 = (uint32_t)(knob / 16);
                master_q12 = (k10 * k10) >> 8;    /* 0 .. ~4096 */
            }
        }

        /* OCT- + OCT+ held 5 s: update mode */
        {
            static uint32_t t0;
            uint32_t both = (1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]);
            if ((fm1_in.buttons & both) != both)
                t0 = fm1_ms;
            else if (fm1_ms - t0 > 5000u)
                enter_uboot("UBOOT");
        }

        wire_service();
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            if (flash_ok)
                ota_session();
            ui_dirty = 1;
        }
        if (usb.uboot_req)
            enter_uboot("UBOOT (USB)");

        /* the wire's actions (taken first: one may arrive while an action waits in the library) */
        {
        uint8_t cmd = wire_cmd;
        wire_cmd = 0;
        switch (cmd) {
        case 1: wifi_power(1); break;
        case 5: wifi_power(0); break;
        case 3: ui_scan(); break;
        case 4: wifi_scan_collect(); break;
        case 0x20:
            if (wire_ssid[0]) {
                nets_front(wire_ssid, wire_pass);
                wire_line("saved");
                if (wifi_st != WS_OFF && wifi_st != WS_CONNECTED)
                    wifi_join(wire_ssid, wire_pass);
            }
            break;
        case 0x22: nets_del(wire_arg); wire_line("deleted"); break;
        case 0x30: ui_leds_off = wire_arg; break;
        case 0x42: {                              /* the radio's analog transmit level, 0..6 (6: the default) */
            extern void wifi_set_pwr(u8 pwr_sel);
            char lb[24];
            wifi_set_pwr(wire_arg);
            snprintf(lb, sizeof lb, "tx power %u", wire_arg);
            shim_log(lb);
            wire_line(lb);
            break;
        }
        case 0x44: {                              /* VDC14 (the RF / analog rail): ANA_CON6[2:0], 3 = 1.40 V, 4 = 1.45 V */
            uint8_t v = fm1_p33_read(6);
            char lb[24];
            fm1_p33_write(6, (uint8_t)((v & ~7u) | (wire_arg & 7u)));
            snprintf(lb, sizeof lb, "vdc14 %02x -> %02x", v, fm1_p33_read(6));
            shim_log(lb);
            wire_line(lb);
            break;
        }
        case 0x40: radio_play(wire_arg); break;
        case 0x45: radio_toggle(); break;
        case 0x47: fx_wire = wire_arg && wire_arg <= FX_N ? 1u << (wire_arg - 1u) : 0u; break;   /* hold FX n */         /* as PLAY on Home */
        case 0x46:                                /* as a knob: level 0..3 (K1..K4), a signed step */
            sel_turn(wire_arg & 3u, (int8_t)(wire_arg2 & 0x40u ? wire_arg2 | 0x80u : wire_arg2));
            break;
        case 0x50: rg_begin(rg_get7(wire_buf, 5)); break;
        case 0x52: rg_end(rg_get7(wire_buf, 5)); break;
        case 0x53:
            if (rg) {
                char lb[64];
                snprintf(lb, sizeof lb, "rg table %u continents %u countries %u cities %u stations",
                         rg->n_cont, rg->n_country, rg->n_city, rg->n_station);
                wire_line(lb);
            } else {
                wire_line("rg none");
            }
            break;
        case 0x41: radio_stop(); break;
        case 0x23:
            if (wire_arg < NET_N && nets.n[wire_arg].ssid[0]) {
                if (wifi_st == WS_OFF)
                    wifi_power(1);
                else
                    wifi_join(nets.n[wire_arg].ssid, nets.n[wire_arg].pass);
            }
            break;
        default: break;
        }
        }

        /* buttons (on press) and the SELECT knob */
        now = plat_buttons();
        down = now & ~prev;
        prev = now;
        turn = plat_enc(EN_SELECT);
        {   /* GLO: WiFi, from anywhere; HOME held: the controls */
            static uint32_t home_t;
            if (down & (1u << B_GLO))
                ui_page_set(ui_page == PG_WIFI ? PG_HOME : PG_WIFI);
            if (now & (1u << B_HOME)) {
                if (down & (1u << B_HOME))
                    home_t = fm1_ms;
                else if (fm1_ms - home_t > 450u && ui_page != PG_HELP && ui_page != PG_PASS)
                    ui_page_set(PG_HELP);
            } else if (ui_page == PG_HELP) {
                ui_page_set(PG_HOME);
                down &= ~(1u << B_HOME);
            }
        }
        {   /* the white keys, held on Home: the punch-in FX (a wire test's 0x47 holds one too) */
            uint32_t keys = ui_page == PG_HOME ? plat_keys() : 0u, h = 0;
            int w;
            for (w = 0; w < 16; w++)
                if (keys >> WHITE_KEY[w] & 1u)
                    h |= 1u << w;
            fx_held = h | fx_wire;
        }
        if (ui_page == PG_HOME) {
            if (down & (1u << B_PLAY))
                radio_toggle();
            if ((down & (1u << B_SEL)) && sel_valid()) {
                br_open();
                ui_page_set(PG_BROWSE);
            }
            sel_turn(0, plat_enc(EN_K1));
            sel_turn(1, plat_enc(EN_K2));
            sel_turn(2, plat_enc(EN_K3));
            sel_turn(3, plat_enc(EN_K4) + turn);
        } else if (ui_page == PG_BROWSE) {
            int n = (int)br_count();
            if (turn) {
                br_cur += turn;
                if (br_cur < 0)
                    br_cur = 0;
                if (br_cur >= n)
                    br_cur = n ? n - 1 : 0;
            }
            if ((down & (1u << B_SEL)) && br_enter())
                ui_page_set(PG_HOME);
            if ((down & (1u << B_HOME)) && br_up())
                ui_page_set(PG_HOME);
            if (down & (1u << B_PLAY))
                radio_toggle();
        } else if (ui_page == PG_WIFI) {
            if (down & (1u << B_PLAY))
                wifi_power(wifi_st == WS_OFF);
            if (down & (1u << B_SEL)) {
                ui_page_set(PG_NETS);
                ui_scan();
            }
            if (down & (1u << B_HOME))
                ui_page_set(PG_HOME);
        } else if (ui_page == PG_NETS) {
            if (turn && scan_n > 0) {
                ui_sel += turn;
                if (ui_sel < 0)
                    ui_sel = 0;
                if (ui_sel >= scan_n)
                    ui_sel = scan_n - 1;
            }
            if (down & (1u << B_SEL))
                ui_scan();
            if (down & (1u << B_HOME))
                ui_page_set(PG_WIFI);
            if ((down & (1u << B_PLAY)) && scan_n > 0) {
                scan_t *s = &scan_list[ui_sel];
                if (wifi_join_ssid(s->ssid, s->secured) == 0)
                    ui_page_set(PG_WIFI);
                else
                    pw_open(s->ssid);
            }
            if ((down & (1u << B_SAVE)) && scan_n > 0)
                pw_open(scan_list[ui_sel].ssid);
        } else if (ui_page == PG_PASS) {
            uint32_t keys = plat_keys(), kdown = keys & ~pw_keys;
            int k;
            pw_keys = keys;
            for (k = 0; k < 27; k++)
                if (kdown >> k & 1u)
                    pw_key(k, pw_layer(now));
            if (down & (1u << B_HOME))
                ui_page_set(PG_NETS);
            if (down & (1u << B_PLAY)) {
                if (pw_len < 8) {
                    snprintf(ui_msg, sizeof ui_msg, "WPA passwords are 8+ characters");
                } else {
                    nets_front(pw_ssid, pw_buf);
                    if (wifi_st == WS_OFF)
                        wifi_power(1);
                    else
                        wifi_join(pw_ssid, pw_buf);
                    ui_page_set(PG_WIFI);
                }
            }
        }

        /* the scan: asked once the radio is up; done when a scan's results arrive (wifi_poll) */
        if (ui_scan_t) {
            static uint8_t asked;
            if (wifi_st >= WS_IDLE && !asked) {
                asked = 1;
                wifi_scan_start();
            }
            if (scan_n >= 0) {
                ui_scan_t = 0;
                asked = 0;
            } else if (fm1_ms - ui_scan_t > 20000u) {
                ui_scan_t = 0;
                asked = 0;
                scan_n = 0;
                snprintf(ui_msg, sizeof ui_msg, "No answer: SEL to scan again");
            }
        }

        /* light what does something here: the page's buttons, and the keys while they type */
        {
            uint32_t lb = 0, lk = 0;
            if (ui_page == PG_HOME) {
                int w;
                lb = 1u << B_PLAY | 1u << B_GLO | (sel_valid() ? 1u << B_SEL : 0u);
                if (radio_st == RS_PLAYING)       /* the FX keys, and the held ones off as they play */
                    for (w = 0; w < 16; w++)
                        if (!(fx_held >> w & 1u))
                            lk |= 1u << WHITE_KEY[w];
            }
            else if (ui_page == PG_BROWSE)
                lb = 1u << B_SEL | 1u << B_HOME | 1u << B_PLAY;
            else if (ui_page == PG_WIFI)
                lb = 1u << B_PLAY | 1u << B_SEL | 1u << B_HOME | 1u << B_GLO;
            else if (ui_page == PG_NETS)
                lb = 1u << B_SEL | 1u << B_HOME | (scan_n > 0 ? 1u << B_PLAY | 1u << B_SAVE : 0u);
            else if (ui_page == PG_PASS) {
                lb = 1u << B_PLAY | 1u << B_HOME | 1u << B_OCTDN | 1u << B_OCTUP;
                lk = (1u << NKEYS) - 1u;
            }
            plat_leds(ui_leds_off ? 0u : lb, ui_leds_off ? 0u : lk);
        }

        if (url_restart) {                        /* wifi_ctl url: the stream again, from the new address */
            url_restart = 0;
            if (radio_st != RS_OFF)
                radio_stop();                     /* radio_ui_service starts it again (the station is unchanged) */
        }
        rg_service();
        radio_ui_service();
        wifi_poll();
        if (wifi_st != WS_CONNECTED && radio_mode != 2 && radio_st != RS_OFF && radio_st != RS_ERROR)
            radio_stop();                         /* the network went: so does the stream */
        radio_poll();

        if (fm1_ms - last_frame >= 100u) {
            last_frame = fm1_ms;
            ui_draw();
        }

#if OM_CDC
        cdc_task();
#endif
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d;
    fm1_time_init();
    fm1_reset_reason();
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 4u) {
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++) *d = 0;
    for (d = _pool_start; d < _pool_end; d++) *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++) *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++) *d = *s;
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_main();
    for (;;) ;
}
