/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Radio on fm1-base: the app's unity build (compiled into fm1-base's, after the platform: gfx.c in scope). The
 * pages (Home, the browser, WiFi, the networks around, a password typed on the keys, held HOME's help), the main
 * loop's work (input, the scan, the lights, the stream), Radio's own commands over USB, and the glue to the player
 * (radio.c), the places (rg.c) and the network side (net/radio_net.c, built apart against lwIP). */

/* ---- the platform as the radio's code knows it (fm1-radio's own platform before fm1-base) */
/* the WiFi states: fm1-base's, in the same order (WIFI_OFF .. WIFI_FAILED) */
enum { WS_OFF, WS_STARTING, WS_IDLE, WS_JOINING, WS_DHCP, WS_CONNECTED, WS_FAILED };
#define wifi_st ((uint8_t)plat_wifi_state())
#define wifi_rssi plat_wifi_rssi()
#define wifi_power(on) plat_wifi_on(on)
#define shim_log(s) plat_log(s)
#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#endif
extern void *malloc(unsigned long n);
extern void free(void *p);
#ifndef PLAT_HOST
extern int snprintf(char *buf, unsigned long sz, const char *f, ...);   /* (fm1-base's WiFi glue: ble/wifi) */
#endif

static int w_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}
static void w_cpy(char *d, const char *s, unsigned max)
{
    unsigned i;
    for (i = 0; i + 1u < max && s && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
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
static const char *wifi_status_str(void)
{
    static const char *const S[] = {"OFF", "STARTING", "ON", "JOINING", "GETTING IP", "CONNECTED", "FAILED"};
    return wifi_st < 7u ? S[wifi_st] : "?";
}

/* ---- the station of the last session (OBJ_RADIO) */
#define RSET_MAGIC 0x52414431u                    /* "RAD1" */
static struct {
    uint32_t magic;
    uint8_t playing, rsv;
    uint16_t country;
    char place[32], st_id[12], st_title[32];
} rset;
static void rset_load(void)
{
    if (plat_store_load(OBJ_RADIO, &rset, sizeof rset) != (int)sizeof rset || rset.magic != RSET_MAGIC) {
        memset(&rset, 0, sizeof rset);
        rset.magic = RSET_MAGIC;
    }
}
static void rset_save(void) { plat_store_save(OBJ_RADIO, &rset, sizeof rset); }

/* ---- the networks around: fm1-base's scan, copied when a scan's results arrive */
#define SCAN_N 12
typedef struct { char ssid[33]; int8_t rssi; uint8_t secured, saved; } scan_t;
static scan_t scan_list[SCAN_N];
static int scan_n = -1;                           /* -1: none yet */
static uint32_t scans_seen;
static void scan_refresh(void)
{
    if (plat_wifi_scans() == scans_seen)
        return;
    scans_seen = plat_wifi_scans();
    for (scan_n = 0; scan_n < SCAN_N; scan_n++) {
        scan_t *s = &scan_list[scan_n];
        int rssi, sec;
        if (!plat_wifi_found(scan_n, s->ssid, &rssi, &sec))
            break;
        s->rssi = (int8_t)rssi;
        s->secured = (uint8_t)sec;
        s->saved = (uint8_t)plat_wifi_kept(s->ssid);
    }
}
/* a join asked while WiFi was still starting: made once it is on */
static char join_ssid[33], join_pass[64];
static uint8_t join_due, join_pass_given;
static void wifi_join_to(const char *ssid, const char *pass)
{
    w_cpy(join_ssid, ssid, sizeof join_ssid);
    w_cpy(join_pass, pass ? pass : "", sizeof join_pass);
    join_pass_given = pass != 0;
    join_due = 1;
    if (wifi_st == WS_OFF)
        wifi_power(1);
}
/* a network around: joined when its password is kept or it is open; -1 = it needs a password */
static int wifi_join_ssid(const char *ssid, int secured)
{
    if (secured && !plat_wifi_kept(ssid))
        return -1;
    wifi_join_to(ssid, 0);
    return 0;
}

/* ---- Radio's own commands over USB: F0 7D 57 <cmd> [data] F7 (tools/radio_ctl.py); the networks are fm1-base's
 * (F0 7D 4C 57: fm1_remote.py wifi ...). Answers: lines F0 7D 57 10 <text> F7, then F0 7D 57 11 F7.
 *   0x10 dump   0x40 n play (0 the stream, 1 fetched and dropped, 2 the built-in MP3)   0x41 stop   0x45 play/stop
 *   0x43 <url as nibbles> the stream's url (none: read it back)   0x46 level step: as a knob (0..3 K1..K4)
 *   0x47 n hold FX n (1..16; 0 let go)   0x48 bpm-40: the tempo   0x53 the places table */
static void wire_send(uint8_t code, const char *s)
{
    uint8_t m[100];
    uint32_t n = 0;
    m[n++] = 0xF0; m[n++] = 0x7D; m[n++] = 0x57; m[n++] = code;
    while (s && *s && n < sizeof m - 2u)
        m[n++] = (uint8_t)*s++ & 0x7Fu;
    m[n++] = 0xF7;
    plat_sysex_send(m, n);
}
static void wire_line(const char *s) { wire_send(0x10, s); }
static void wire_end(void) { wire_send(0x11, 0); }
static volatile uint8_t wire_cmd, wire_arg, wire_arg2;

static const uint8_t WHITE_KEY[16] = PLAT_WHITE_KEYS;   /* F3 .. G5 */
static uint32_t fx_wire;
static volatile uint8_t url_restart;
static uint32_t main_loops;

#include "inflate.c"
#include "rg.c"
#include "radio.c"

/* tap tempo (FX on Home): the last taps, the tempo they make, when the screen stops showing it */
static uint32_t tap_t[4], tap_n, tempo_bpm = 120, tempo_t0, tempo_shown_until;
static void tempo_set(uint32_t beat_ms, uint32_t t0)
{
    if (beat_ms < 250u)
        beat_ms = 250u;                           /* 240 BPM */
    if (beat_ms > 1500u)
        beat_ms = 1500u;                          /* 40 BPM */
    fx_beat = beat_ms * 441u / 10u;
    tempo_bpm = (60000u + beat_ms / 2u) / beat_ms;
    tempo_t0 = t0;
    tempo_shown_until = plat_ms() + 2000u;
}
static void tempo_tap(void)
{
    uint32_t now = plat_ms(), n;
    if (tap_n && now - tap_t[(tap_n - 1u) % 4u] > 2000u)
        tap_n = 0;                                /* a pause: a new count */
    tap_t[tap_n % 4u] = now;
    tap_n++;
    n = tap_n < 4u ? tap_n : 4u;
    if (n >= 2u) {                                /* the mean of the last intervals */
        uint32_t first = tap_t[(tap_n - n) % 4u];
        tempo_set((now - first) / (n - 1u), now);
    } else {
        tempo_t0 = now;
        tempo_shown_until = now + 2000u;
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
static uint8_t ui_page, ui_dirty = 1;
static int ui_sel, ui_top;
static uint32_t ui_scan_t;                        /* the scan asked for (ms), 0 = none waiting */
static char ui_msg[40];
#define UI_ROWS 14
static char ui_key[UI_ROWS][80] __attribute__((section(".pool")));

static void ui_row(int slot, uint32_t y, uint32_t h, const char *l, const char *r, uint16_t lc, uint16_t rc,
                   uint16_t bg)
{
    char key[80];
    int32_t rw;
    snprintf(key, sizeof key, "%u%s|%s|%x%x%x", (unsigned)y, l, r, lc, rc, bg);
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
    uint32_t ip = plat_wifi_ip();
    ui_title(0, "WiFi", "");
    ui_row(1, 48, 26, "WiFi", wifi_status_str(), K_DIM, wc, K_BG);
    ssid_show(ssid, plat_wifi_ssid()[0] ? plat_wifi_ssid() : "-", sizeof ssid);
    if (wifi_st == WS_OFF)
        ssid[0] = 0;
    ui_row(2, 74, 26, "Network", ssid, K_DIM, K_TEXT, K_BG);
    v[0] = 0;
    if (ip)
        snprintf(v, sizeof v, "%u.%u.%u.%u", (unsigned)(ip & 255u), (unsigned)(ip >> 8 & 255u),
                 (unsigned)(ip >> 16 & 255u), (unsigned)(ip >> 24));
    ui_row(3, 100, 26, "Address", wifi_st == WS_FAILED ? plat_wifi_why() : v, K_DIM,
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
        ui_row(5, 152, 26, "Stream", nm, K_DIM, rc, K_BG);
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
                if (w_eq(s->ssid, plat_wifi_ssid()) && wifi_st == WS_CONNECTED)
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

/* ---- the password page: the 27 keys type (F3 = a ... E5 = z, G5 = delete); hold OCT- for capitals, OCT+ for
 * digits (F3 .. D4) and space / symbols (D#4 .. F#5) */
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
static void pw_open(const char *ssid)             /* (a kept password is not shown: it is typed again) */
{
    w_cpy(pw_ssid, ssid, sizeof pw_ssid);
    pw_buf[0] = 0;
    pw_len = 0;
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
    snprintf(key, sizeof key, "P%d%x", layer, (unsigned)held);
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
    {   /* the end of what is typed, with a cursor */
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

static const char *places_status(char *b, unsigned max);   /* places.c */
#include "radio_ui.c"
#include "places.c"

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
    scans_seen = plat_wifi_scans();               /* the next scan's results */
    ui_msg[0] = 0;
    if (wifi_st == WS_OFF)
        wifi_power(1);                            /* the scan asks for the radio */
    ui_scan_t = plat_ms() ? plat_ms() : 1;
}

/* ---- Radio's commands over USB */
static void wire_dump(void)
{
    char b[96];
    snprintf(b, sizeof b, "up %u ms  wifi %s  loops %u", (unsigned)plat_ms(), wifi_status_str(), (unsigned)main_loops);
    wire_line(b);
    radio_ui_dump();
#ifndef PLAT_HOST
    {
        extern void shim_os_dump(void);
        shim_os_dump();                           /* each task, what it waits for, its deepest stack: into the log */
    }
    {
        extern uint32_t shim_heap_used, shim_heap_peak;
        extern const uint32_t shim_heap_size;
        snprintf(b, sizeof b, "heap %u used, %u peak, of %u", (unsigned)shim_heap_used, (unsigned)shim_heap_peak,
                 (unsigned)shim_heap_size);
        wire_line(b);
    }
#endif
    snprintf(b, sizeof b, "radio %s  net %d  buf %u/%u  pcm %u  fr %u  %uHz %ukbps", radio_status_str(),
             radio_net_state, (unsigned)(radio_wr - radio_rd), (unsigned)NET_RING, (unsigned)(pcm_w - pcm_r),
             (unsigned)radio_frames, (unsigned)radio_hz, (unsigned)radio_kbps);
    wire_line(b);
    snprintf(b, sizeof b, "  under %u  rebuf %u  reconn %u  %s%s", (unsigned)radio_underruns,
             (unsigned)radio_rebuffers, (unsigned)radio_reconnects, dec_kind == 2 ? "AAC " : dec_kind == 1 ? "MP3 " : "",
             radio_why);
    wire_line(b);
    snprintf(b, sizeof b, "  aac bad frames %u", (unsigned)radio_aac_errs);
    wire_line(b);
    {   /* rates since the last dump: what came in, what was decoded, how often the main loop went round */
        static uint32_t t_last, wr_last, fr_last, loops_last;
        uint32_t dt = plat_ms() - t_last ? plat_ms() - t_last : 1u, spf;
        snprintf(b, sizeof b, "  decode %u us a frame, %u at most", radio_dec_n ? (unsigned)(radio_dec_us / radio_dec_n) : 0u,
                 (unsigned)radio_dec_max);
        wire_line(b);
        radio_dec_us = radio_dec_n = radio_dec_max = 0;
        spf = dec_kind != 2 ? 1152u : aac_ext->aacPlusUpsamplingFactor == 2 ? 2048u : 1024u;   /* samples a frame */
        snprintf(b, sizeof b, "  mode %u  in %u kbit/s  decoded %u fr/s (need %u)  loop %u/s", radio_mode,
                 (unsigned)((radio_wr - wr_last) * 8u / dt), (unsigned)((radio_frames - fr_last) * 1000u / dt),
                 (unsigned)(radio_hz / spf), (unsigned)((main_loops - loops_last) * 1000u / dt));
        wire_line(b);
        t_last = plat_ms();
        wr_last = radio_wr;
        fr_last = radio_frames;
        loops_last = main_loops;
    }
}

int app_sysex(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (n < 3u || p[0] != 0x7Du || p[1] != 0x57u)
        return 0;
    switch (p[2]) {
    case 0x10:
        wire_dump();
        break;
    case 0x43:
        if (n <= 3u) {                            /* none: read it back */
            wire_line(radio_url);
        } else {                                  /* the stream's url, as nibbles */
            uint32_t k = 0;
            for (i = 3; i + 1u < n && k + 1u < sizeof radio_url_buf; i += 2)
                radio_url_buf[k++] = (char)((p[i] & 15u) << 4 | (p[i + 1] & 15u));
            radio_url_buf[k] = 0;
            /* as the station's own link (the tuning plays play_url): it plays until the next tune */
            w_cpy(play_url, k ? radio_url_buf : RADIO_URL, sizeof play_url);
            radio_url = play_url;
            radio_want = 1;
            url_restart = 1;                      /* the main loop restarts the stream on it */
        }
        break;
    case 0x54:                                    /* the places fetched again */
        wire_cmd = 0x54;
        break;
    case 0x53:
        if (rg) {
            char lb[64];
            snprintf(lb, sizeof lb, "rg places: %u continents %u countries %u places", rg->n_cont, rg->n_country,
                     rg->n_place);
            wire_line(lb);
        } else {
            wire_line("rg none");
        }
        break;
    default:                                      /* the main loop's */
        wire_arg = n > 3u ? p[3] : 0;
        wire_arg2 = n > 4u ? p[4] : 0;
        wire_cmd = p[2];
        break;
    }
    wire_end();
    return 1;
}

static void wire_actions(void)
{
    uint8_t cmd = wire_cmd;
    wire_cmd = 0;
    switch (cmd) {
    case 0x40: radio_play(wire_arg); break;
    case 0x41: radio_stop(); break;
    case 0x45: radio_toggle(); break;
    case 0x46:                                    /* as a knob: level 0..3 (K1..K4), a signed step */
        sel_turn(wire_arg & 3u, (int8_t)(wire_arg2 & 0x40u ? wire_arg2 | 0x80u : wire_arg2));
        break;
    case 0x47: fx_wire = wire_arg && wire_arg <= FX_N ? 1u << (wire_arg - 1u) : 0u; break;
    case 0x48: tempo_set(60000u / (wire_arg ? wire_arg + 40u : 120u), plat_ms()); break;
    case 0x54: places_refresh(); break;
    default: break;
    }
}

/* ---- the contract */
void app_init(void)
{
    arena_init();                                 /* first, while the heap is whole (radio.c) */
    rg_open();
    fx_reset();
    rset_load();
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    (void)master_q12;                             /* (radio_render reads MASTER itself) */
    radio_render(lr, n);
}
void app_panic(void) { radio_st = RS_OFF; }
void app_say(const char *a, const char *b)
{
    snprintf(ui_msg, sizeof ui_msg, "%s %s", a ? a : "", b ? b : "");
    ui_dirty = 1;
}

void app_ui_init(void)
{
    radio_ui_restore();                           /* the place and station of the last session, playing if it was */
    if (plat_wifi_saved(0, 0)) {                  /* a radio: WiFi on by itself when it knows a network */
        wifi_power(1);
    } else {                                      /* the first start: straight to the networks around */
        ui_page = PG_NETS;
        ui_scan();
        snprintf(ui_msg, sizeof ui_msg, "Pick your WiFi: SELECT, then PLAY");
    }
    ui_dirty = 1;
}

static uint32_t btn_prev;
static void input(void)
{
    uint32_t now = plat_buttons(), down = now & ~btn_prev;
    int32_t turn = plat_enc(EN_SELECT);
    btn_prev = now;
    {   /* GLO: WiFi, from anywhere; HOME held: the controls */
        static uint32_t home_t;
        if (down & (1u << B_GLO))
            ui_page_set(ui_page == PG_WIFI ? PG_HOME : PG_WIFI);
        if (now & (1u << B_HOME)) {
            if (down & (1u << B_HOME))
                home_t = plat_ms();
            else if (plat_ms() - home_t > 450u && ui_page != PG_HELP && ui_page != PG_PASS)
                ui_page_set(PG_HELP);
        } else if (ui_page == PG_HELP) {
            ui_page_set(PG_HOME);
            down &= ~(1u << B_HOME);
        }
    }
    {   /* the white keys, held on Home: the punch-in FX (a wire command's 0x47 holds one too) */
        uint32_t keys = ui_page == PG_HOME ? plat_keys() : 0u, h = 0;
        int w;
        for (w = 0; w < 16; w++)
            if (keys >> WHITE_KEY[w] & 1u)
                h |= 1u << w;
        fx_held = h | fx_wire;
    }
    if (ui_page == PG_HOME) {
        if (down & (1u << B_FX))
            tempo_tap();
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
                wifi_join_to(pw_ssid, pw_buf);    /* kept once it joins */
                ui_page_set(PG_WIFI);
            }
        }
    }
}
void app_input_only(void) { input(); }

/* light what does something here: the page's buttons, and the keys while they type */
static void lights(void)
{
    uint32_t lb = 0, lk = 0;
    if (ui_page == PG_HOME) {
        int w;
        lb = 1u << B_PLAY | 1u << B_GLO | (sel_valid() ? 1u << B_SEL : 0u);
        if ((plat_ms() - tempo_t0) % (60000u / tempo_bpm) < 90u)
            lb |= 1u << B_FX;                     /* FX flashes on the beat */
        if (radio_st == RS_PLAYING)               /* the FX keys, and the held ones off as they play */
            for (w = 0; w < 16; w++)
                if (!(fx_held >> w & 1u))
                    lk |= 1u << WHITE_KEY[w];
    } else if (ui_page == PG_BROWSE) {
        lb = 1u << B_SEL | 1u << B_HOME | 1u << B_PLAY;
    } else if (ui_page == PG_WIFI) {
        lb = 1u << B_PLAY | 1u << B_SEL | 1u << B_HOME | 1u << B_GLO;
    } else if (ui_page == PG_NETS) {
        lb = 1u << B_SEL | 1u << B_HOME | (scan_n > 0 ? 1u << B_PLAY | 1u << B_SAVE : 0u);
    } else if (ui_page == PG_PASS) {
        lb = 1u << B_PLAY | 1u << B_HOME | 1u << B_OCTDN | 1u << B_OCTUP;
        lk = (1u << NKEYS) - 1u;
    }
    plat_leds(lb, lk);
}

void app_frame(void)
{
    static uint32_t last_draw;
    input();
    wire_actions();
    scan_refresh();
    if (join_due && wifi_st >= WS_IDLE && wifi_st != WS_STARTING) {   /* a join asked while WiFi started */
        join_due = 0;
        plat_wifi_join(join_ssid, join_pass_given ? join_pass : 0);
    }
    /* the scan: asked once the radio is up; done when a scan's results arrive */
    if (ui_scan_t) {
        static uint8_t asked;
        if (wifi_st >= WS_IDLE && !asked) {
            asked = 1;
            plat_wifi_scan();
        }
        if (scan_n >= 0) {
            ui_scan_t = 0;
            asked = 0;
        } else if (plat_ms() - ui_scan_t > 20000u) {
            ui_scan_t = 0;
            asked = 0;
            scan_n = 0;
            snprintf(ui_msg, sizeof ui_msg, "No answer: SEL to scan again");
        }
    }
    lights();
    if (plat_ms() - last_draw >= 100u) {
        last_draw = plat_ms();
        ui_draw();
    }
}

/* every pass of the main loop: the stream decoded, the tuning */
void app_loop(void)
{
    main_loops++;
    if (url_restart) {                            /* a new url over USB: the stream again, from it */
        url_restart = 0;
        if (radio_st != RS_OFF)
            radio_stop();                         /* radio_ui_service starts it again (the station is unchanged) */
    }
    places_poll();                                /* no places yet: fetched from Radio Garden (places.c) */
    radio_ui_service();
    if (wifi_st != WS_CONNECTED && radio_mode != 2 && radio_st != RS_OFF && radio_st != RS_ERROR)
        radio_stop();                             /* the network went: so does the stream */
    radio_poll();
}

#ifdef PLAT_WEB
/* the browser emulator: the page does the network (web/emu/app.js: fetch, through a proxy for Radio Garden, which
 * answers no other site), the worklet hands its bytes over (web/emu/app_worklet.js), and the app plays them as on
 * the FM-1: the same decoders, the same ring. A request is a generation number the worklet watches; what comes back
 * for an older one is dropped. Strings travel through web_net_scratch, NUL-separated. */
#pragma push_macro("__attribute__")              /* (plat_host.c defines it away) */
#undef __attribute__
#define WEB_EXPORT __attribute__((used, visibility("default")))
volatile int radio_net_state, rgn_state, radio_net_aac;
volatile uint32_t radio_rd, radio_wr, rgn_n;
char radio_net_why[32], radio_net_name[48], rgn_why[32];
static rgn_station_t web_st[RGN_MAX];
rgn_station_t *rgn_list = web_st;
static uint8_t web_scratch[32768];
static char web_url[512], web_place[32], web_country[32];
static uint8_t *web_ring;
static uint32_t web_ring_size, web_net_g, web_rgn_g, web_rgp_g;
static int web_running;
WEB_EXPORT uint8_t *web_net_scratch(void) { return web_scratch; }
WEB_EXPORT uint32_t web_scratch_size(void) { return sizeof web_scratch; }

/* the stream: wanted while web_net_want(), from web_net_url() */
int radio_net_start(const char *u, uint8_t *buf, uint32_t size)
{
    if (web_running)
        return -1;
    w_cpy(web_url, u, sizeof web_url);
    web_ring = buf;
    web_ring_size = size;
    radio_rd = radio_wr = 0;
    radio_net_why[0] = radio_net_name[0] = 0;
    radio_net_aac = 0;
    radio_net_state = RN_CONNECTING;
    web_running = 1;
    web_net_g++;
    return 0;
}
void radio_net_stop(void)
{
    if (!web_running)
        return;
    web_running = 0;
    radio_net_state = RN_IDLE;
    web_net_g++;                                  /* the page lets go of the fetch */
}
int radio_net_running(void) { return web_running; }
int radio_net_tls(void) { return 0; }
WEB_EXPORT uint32_t web_net_gen(void) { return web_net_g; }
WEB_EXPORT int web_net_want(void) { return web_running; }
WEB_EXPORT char *web_net_url(void) { return web_url; }
WEB_EXPORT uint32_t web_net_room(void) { return web_running ? web_ring_size - (radio_wr - radio_rd) : 0; }
/* the stream's answer: playing, AAC or MP3, the station's name (scratch) if the page could read it */
WEB_EXPORT void web_net_head(uint32_t g, int aac)
{
    if (g != web_net_g || !web_running)
        return;
    radio_net_aac = aac;
    w_cpy(radio_net_name, (const char *)web_scratch, sizeof radio_net_name);
    radio_net_state = RN_STREAMING;
}
/* n bytes of the stream from scratch into the ring, as many as fit; how many were taken (all, for an old one) */
WEB_EXPORT uint32_t web_net_put(uint32_t g, uint32_t n)
{
    uint32_t room, k, at;
    if (g != web_net_g || !web_running)
        return n;
    room = web_ring_size - (radio_wr - radio_rd);
    if (n > room)
        n = room;
    for (k = 0; k < n; k++) {
        at = (radio_wr + k) % web_ring_size;
        web_ring[at] = web_scratch[k];
    }
    RING_PUBLISH();
    radio_wr += n;
    return n;
}
/* it ended: state RN_ERROR, RN_ENDED or RN_UNPLAYABLE, the reason in scratch */
WEB_EXPORT void web_net_end(uint32_t g, int state)
{
    if (g != web_net_g || !web_running)
        return;
    w_cpy(radio_net_why, (const char *)web_scratch, sizeof radio_net_why);
    radio_net_state = state;
    web_running = 0;
}

/* Radio Garden: a place's stations */
int rgn_start(const char *place, const char *country)
{
    if (rgn_state == RGN_BUSY)
        return -1;
    w_cpy(web_place, place, sizeof web_place);
    w_cpy(web_country, country, sizeof web_country);
    rgn_n = 0;
    rgn_why[0] = 0;
    rgn_state = RGN_BUSY;
    web_rgn_g++;
    return 0;
}
WEB_EXPORT uint32_t web_rgn_gen(void) { return web_rgn_g; }
WEB_EXPORT int web_rgn_busy(void) { return rgn_state == RGN_BUSY; }
WEB_EXPORT char *web_rgn_place(void) { return web_place; }
WEB_EXPORT char *web_rgn_country(void) { return web_country; }
/* a station: "id\0title\0" in scratch */
WEB_EXPORT void web_rgn_add(uint32_t g)
{
    const char *id = (const char *)web_scratch;
    if (g != web_rgn_g || rgn_state != RGN_BUSY || rgn_n >= RGN_MAX)
        return;
    w_cpy(web_st[rgn_n].id, id, sizeof web_st[rgn_n].id);
    w_cpy(web_st[rgn_n].title, id + strlen(id) + 1u, sizeof web_st[rgn_n].title);
    rgn_n++;
}
WEB_EXPORT void web_rgn_end(uint32_t g, int ok)
{
    if (g != web_rgn_g || rgn_state != RGN_BUSY)
        return;
    if (!ok)
        w_cpy(rgn_why, (const char *)web_scratch, sizeof rgn_why);
    rgn_state = ok || rgn_n ? RGN_DONE : RGN_ERROR;
}

/* Radio Garden: every place, for the table the app builds (places.c), handed over a batch at a time */
volatile int rgp_state;
volatile uint32_t rgp_bytes;
char rgp_why[32];
static void (*web_rgp_cb)(const char *title, const char *country);
int rgp_start(void (*cb)(const char *title, const char *country))
{
    if (rgp_state == 1)                           /* RGP_BUSY */
        return -1;
    web_rgp_cb = cb;
    rgp_bytes = 0;
    rgp_why[0] = 0;
    rgp_state = 1;
    web_rgp_g++;
    return 0;
}
WEB_EXPORT uint32_t web_rgp_gen(void) { return web_rgp_g; }
WEB_EXPORT int web_rgp_busy(void) { return rgp_state == 1; }
/* n bytes of "title\0country\0" pairs in scratch, as received from bytes more of the list */
WEB_EXPORT void web_rgp_feed(uint32_t g, uint32_t n, uint32_t bytes)
{
    uint32_t i = 0;
    if (g != web_rgp_g || rgp_state != 1)
        return;
    while (i < n) {
        const char *t = (const char *)web_scratch + i, *c = t + strlen(t) + 1u;
        web_rgp_cb(t, c);
        i = (uint32_t)(c - (const char *)web_scratch) + (uint32_t)strlen(c) + 1u;
    }
    rgp_bytes += bytes;
}
WEB_EXPORT void web_rgp_end(uint32_t g, int ok)
{
    if (g != web_rgp_g || rgp_state != 1)
        return;
    if (!ok)
        w_cpy(rgp_why, (const char *)web_scratch, sizeof rgp_why);
    rgp_state = ok ? 2 : 3;                       /* RGP_DONE, RGP_ERROR */
}
#pragma pop_macro("__attribute__")
#elif defined PLAT_HOST
/* the host simulator has no lwIP (net/radio_net.c is the device's): Radio Garden answers with two stations, and a
 * stream cannot start */
volatile int radio_net_state, rgn_state, radio_net_aac;
volatile uint32_t radio_rd, radio_wr, rgn_n;
char radio_net_why[32], radio_net_name[48], rgn_why[32];
static rgn_station_t host_st[2] = {{"aaaa1111", "Host FM"}, {"bbbb2222", "Simulated Radio"}};
rgn_station_t *rgn_list = host_st;
int radio_net_start(const char *u, uint8_t *buf, uint32_t size)
{
    (void)u, (void)buf, (void)size;
    radio_net_state = RN_ERROR;
    w_cpy(radio_net_why, "no network in the simulator", sizeof radio_net_why);
    return 0;
}
void radio_net_stop(void) {}
int radio_net_running(void) { return 0; }
int radio_net_tls(void) { return 0; }
int rgn_start(const char *place, const char *country)
{
    (void)place, (void)country;
    rgn_n = 2;
    rgn_state = RGN_DONE;
    return 0;
}
/* Radio Garden's places: a few, in no order, as the list has them (and a country no continent knows) */
volatile int rgp_state;
volatile uint32_t rgp_bytes;
char rgp_why[32];
int rgp_start(void (*cb)(const char *title, const char *country))
{
    static const char *const P[][2] = {
        {"Utrecht", "Netherlands"}, {"Adrar", "Algeria"}, {"Amsterdam", "Netherlands"}, {"  Rotterdam ", "Netherlands"},
        {"Oran", "Algeria"}, {"Amsterdam", "Netherlands"}, {"Pago Pago", "American Samoa"}, {"zwolle", "Netherlands"},
        {"Lost City", "Atlantis"}, {"Brisbane", "Australia"}, {"Adelaide", "Australia"}, {"Algiers", "Algeria"}};
    unsigned i;
    for (i = 0; i < sizeof P / sizeof P[0]; i++)
        cb(P[i][0], P[i][1]);
    rgp_bytes += 1900u * 1024u;
    rgp_state = 2;                                /* RGP_DONE */
    return 0;
}
#endif
