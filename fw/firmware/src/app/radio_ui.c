/* The radio's pages, in Polly / X0X's grammar: HOME the station playing (KNOB 1..4 a world dial: continent,
 * country, city, station; SELECT steps the stations; it tunes a moment after the turning stops), SEL the world
 * as a list (SELECT moves, SEL opens, HOME goes up), PLAY plays / stops, hold HOME for the help, GLO WiFi.
 * Part of the unity build (main_wifi.c, after rg.c and the ui_* drawing). */

/* ---- the selection: global indices into the table's countries / cities / stations */
static uint16_t sel_cont, sel_country, sel_city, sel_station;
static uint8_t tune_want;                         /* 1: tune sel_station once the turning has stopped */
static uint32_t tune_at;                          /* when (ms) */
static uint16_t tuned_station = 0xFFFFu;          /* what radio_url points at */
static uint8_t radio_want;                        /* PLAY's: the radio should be on (kept across a power cycle) */

static int sel_valid(void) { return rg && rg->n_station; }

/* the selection from a station: its city, country, continent (the parents, searched) */
static void sel_from_station(uint32_t st)
{
    uint32_t c, co, ci;
    if (!sel_valid() || st >= rg->n_station)
        st = 0;
    sel_station = (uint16_t)st;
    for (ci = 0; ci < rg->n_city; ci++)
        if (st >= rg_city(ci)->first && st < (uint32_t)rg_city(ci)->first + rg_city(ci)->n)
            break;
    for (co = 0; co < rg->n_country; co++)
        if (ci >= rg_country(co)->first && ci < (uint32_t)rg_country(co)->first + rg_country(co)->n)
            break;
    for (c = 0; c < rg->n_cont; c++)
        if (co >= rg_cont(c)->first && co < (uint32_t)rg_cont(c)->first + rg_cont(c)->n)
            break;
    sel_city = (uint16_t)ci;
    sel_country = (uint16_t)co;
    sel_cont = (uint16_t)c;
}

/* a step d within the parent's children [first, first + n), wrapping */
static uint16_t step_in(uint32_t cur, uint32_t first, uint32_t n, int32_t d)
{
    int32_t i = (int32_t)(cur - first) + d;
    if (!n)
        return (uint16_t)first;
    i %= (int32_t)n;
    if (i < 0)
        i += (int32_t)n;
    return (uint16_t)(first + (uint32_t)i);
}

/* a knob: level 0 continent .. 3 station; the levels below go to their first child */
static void sel_turn(int level, int32_t d)
{
    if (!sel_valid() || !d)
        return;
    if (level == 0)
        sel_cont = step_in(sel_cont, 0, rg->n_cont, d);
    if (level <= 0)
        sel_country = rg_cont(sel_cont)->first;
    if (level == 1)
        sel_country = step_in(sel_country, rg_cont(sel_cont)->first, rg_cont(sel_cont)->n, d);
    if (level <= 1)
        sel_city = rg_country(sel_country)->first;
    if (level == 2)
        sel_city = step_in(sel_city, rg_country(sel_country)->first, rg_country(sel_country)->n, d);
    if (level <= 2)
        sel_station = rg_city(sel_city)->first;
    if (level == 3)
        sel_station = step_in(sel_station, rg_city(sel_city)->first, rg_city(sel_city)->n, d);
    tune_want = 1;
    tune_at = fm1_ms + 900u;                      /* a dial: it tunes when the turning stops */
}

/* kept in the networks' object (nets.last / nets.playing, wifi.c) */
static void radio_remember(void)
{
    if (nets.last != sel_station || nets.playing != radio_want) {
        nets.last = sel_station;
        nets.playing = radio_want;
        nets_save();
    }
}

/* the main loop's: tune, start and stop as asked, once the network side is free */
static void radio_ui_service(void)
{
    if (!sel_valid())
        return;
    if (tune_want && (int32_t)(fm1_ms - tune_at) >= 0) {
        tune_want = 0;
        if (sel_station != tuned_station && radio_st != RS_OFF && radio_st != RS_ERROR)
            radio_stop();
        tuned_station = sel_station;
        radio_url = rg_str(rg_station(sel_station)->url);
        radio_remember();
    }
    if (radio_want && (radio_st == RS_OFF || radio_st == RS_ERROR) && wifi_st == WS_CONNECTED &&
        !radio_net_running() && !tune_want) {
        static uint32_t last_try;
        if (radio_st == RS_OFF || fm1_ms - last_try > 10000u) {   /* a stopped stream: try again now and then */
            last_try = fm1_ms;
            if (tuned_station != sel_station) {
                tuned_station = sel_station;
                radio_url = rg_str(rg_station(sel_station)->url);
            }
            radio_play(0);
        }
    }
    if (!radio_want && radio_st != RS_OFF)
        radio_stop();
}

static void radio_toggle(void)
{
    radio_want = !radio_want;
    if (radio_want && wifi_st == WS_OFF)
        wifi_power(1);
    radio_remember();
}

/* ---- drawing */
static void ui_text_big(int slot, uint32_t y, const char *t, uint16_t c)   /* the station's name, as large as fits */
{
    char key[80];
    const felucca_font_t *f = text_w(&FONT_M, t) <= 208 ? &FONT_M : &FONT_B;
    snprintf(key, sizeof key, "B%s|%x", t, c);
    if (w_eq(ui_key[slot], key))
        return;
    memcpy(ui_key[slot], key, sizeof key);
    cv_begin(240, 30, K_BG);
    cv_text(16, (int32_t)(30 - f->h) / 2, f, t, c);
    cv_blit(0, y);
}

static uint32_t strlen_(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* a knob's cell: its label small, its value under it (KNOB n in the corner) */
static void ui_cell(int slot, uint32_t x, uint32_t y, const char *knob, const char *label, const char *val, int hi)
{
    char key[80], v[24];
    snprintf(key, sizeof key, "C%s|%s|%d", label, val, hi);
    if (w_eq(ui_key[slot], key))
        return;
    memcpy(ui_key[slot], key, sizeof key);
    cv_begin(112, 44, hi ? K_PANEL : K_BG);
    cv_text(6, 2, &FONT_XS, label, K_DIM);
    cv_text(106 - text_w(&FONT_XS, knob), 2, &FONT_XS, knob, K_LINE);
    w_cpy(v, val, sizeof v);
    while (v[0] && text_w(&FONT_S, v) > 100)      /* cut to fit */
        v[strlen_(v) - 1] = 0;
    cv_text(6, 22, &FONT_S, v, K_TEXT);
    cv_blit(x, y);
}

static void ui_home(void)
{
    char hint[24], line[48];
    uint16_t sc;
    if (wifi_st == WS_CONNECTED)
        snprintf(hint, sizeof hint, "WiFi %d", wifi_rssi);
    else
        w_cpy(hint, wifi_st == WS_OFF ? "WiFi off" : wifi_st == WS_FAILED ? "no WiFi" : "WiFi ...", sizeof hint);
    ui_title(0, "Radio", hint);
    if (!sel_valid()) {
        ui_text_big(1, 44, "No stations yet", K_TEXT);
        ui_row(2, 78, 22, "Load them from the computer:", "", K_DIM, K_DIM, K_BG);
        ui_row(3, 100, 22, "tools/rg_sync.py, then rgload", "", K_DIM, K_DIM, K_BG);
        ui_row(13, 214, 26, "GLO  wifi", "", K_DIM, K_DIM, K_PANEL);
        return;
    }
    sc = radio_st == RS_PLAYING ? K_TEAL : radio_st == RS_ERROR ? K_CORAL : K_TEXT;
    ui_text_big(1, 42, rg_str(rg_station(sel_station)->name), tune_want ? K_CORAL : sc);
    snprintf(line, sizeof line, "%s, %s", rg_str(rg_city(sel_city)->name), rg_str(rg_country(sel_country)->name));
    ui_row(2, 74, 22, line, "", K_DIM, K_DIM, K_BG);
    if (!radio_want)
        w_cpy(line, "stopped", sizeof line);
    else if (wifi_st != WS_CONNECTED)
        w_cpy(line, "waiting for WiFi", sizeof line);
    else if (radio_st == RS_PLAYING)
        snprintf(line, sizeof line, "playing  %u kbit/s", radio_kbps);
    else if (radio_st == RS_BUFFERING)
        snprintf(line, sizeof line, "buffering %u%%", (radio_wr - radio_rd) * 100u / NET_RING);
    else if (radio_st == RS_ERROR)
        snprintf(line, sizeof line, "%s: trying again", radio_why);
    else
        w_cpy(line, "tuning...", sizeof line);
    if (!fx_held && (int32_t)(tempo_shown_until - fm1_ms) > 0) {   /* just tapped: the tempo */
        snprintf(line, sizeof line, "tempo %u BPM", (unsigned)tempo_bpm);
        ui_row(3, 96, 22, line, "", K_TEAL, K_DIM, K_BG);
    } else if (fx_held) {                         /* the held effects instead of the status */
        uint32_t k, o = 0;
        line[0] = 0;
        for (k = 0; k < FX_N && o + 2u < sizeof line; k++)
            if (fx_held >> k & 1u)
                o += (uint32_t)snprintf(line + o, sizeof line - o, "%s%s", o ? " + " : "", FX_NAME[k]);
        ui_row(3, 96, 22, line, "", K_CORAL, K_DIM, K_BG);
    } else {
        ui_row(3, 96, 22, line, "", radio_st == RS_ERROR ? K_CORAL : K_DIM, K_DIM, K_BG);
    }
    {
        char v[24];
        uint32_t ci = sel_city, n = rg_city(ci)->n;
        ui_cell(4, 8, 124, "K1", "Continent", rg_str(rg_cont(sel_cont)->name), 0);
        ui_cell(5, 120, 124, "K2", "Country", rg_str(rg_country(sel_country)->name), 0);
        ui_cell(6, 8, 168, "K3", "City", rg_str(rg_city(sel_city)->name), 0);
        snprintf(v, sizeof v, "%u of %u", (unsigned)(sel_station - rg_city(ci)->first + 1u), (unsigned)n);
        ui_cell(7, 120, 168, "K4", "Station", v, 0);
    }
    ui_row(13, 214, 26, radio_want ? "PLAY stop  SEL browse" : "PLAY play  SEL browse", "GLO wifi", K_DIM, K_DIM,
           K_PANEL);
}

/* ---- the browser: a level of the world as a list */
static uint8_t br_level;                          /* 0 continents .. 3 stations */
static int br_cur, br_top;

static uint32_t br_count(void)
{
    return br_level == 0 ? rg->n_cont : br_level == 1 ? rg_cont(sel_cont)->n
         : br_level == 2 ? rg_country(sel_country)->n : rg_city(sel_city)->n;
}
static uint32_t br_first(void)
{
    return br_level == 0 ? 0u : br_level == 1 ? rg_cont(sel_cont)->first
         : br_level == 2 ? rg_country(sel_country)->first : rg_city(sel_city)->first;
}
static const char *br_name(uint32_t i)
{
    return br_level == 0 ? rg_str(rg_cont(i)->name) : br_level == 1 ? rg_str(rg_country(i)->name)
         : br_level == 2 ? rg_str(rg_city(i)->name) : rg_str(rg_station(i)->name);
}
static uint32_t br_sel(void)                     /* the selection at this level (a global index) */
{
    return br_level == 0 ? sel_cont : br_level == 1 ? sel_country : br_level == 2 ? sel_city : sel_station;
}

static void br_open(void)                         /* the browser on the station's level, at the station */
{
    br_level = 3;
    br_cur = (int)(sel_station - rg_city(sel_city)->first);
    br_top = 0;
}

static void ui_browse(void)
{
    static const char *const T[4] = {"World", "", "", ""};
    char title[40];
    int i, n = (int)br_count();
    if (br_level == 0)
        w_cpy(title, T[0], sizeof title);
    else
        w_cpy(title, br_level == 1 ? rg_str(rg_cont(sel_cont)->name) : br_level == 2 ?
              rg_str(rg_country(sel_country)->name) : rg_str(rg_city(sel_city)->name), sizeof title);
    ui_title(0, title, "");
    if (br_cur >= n)
        br_cur = n ? n - 1 : 0;
    if (br_cur < br_top)
        br_top = br_cur;
    if (br_cur >= br_top + 7)
        br_top = br_cur - 6;
    for (i = 0; i < 7; i++) {
        int k = br_top + i;
        char r[16];
        uint32_t g = br_first() + (uint32_t)k;
        r[0] = 0;
        if (k < n && br_level < 3) {              /* how many below */
            uint32_t m = br_level == 0 ? rg_cont(g)->n : br_level == 1 ? rg_country(g)->n : rg_city(g)->n;
            snprintf(r, sizeof r, "%u", (unsigned)m);
        }
        if (k < n && br_level == 3 && g == tuned_station && radio_st == RS_PLAYING)
            w_cpy(r, "playing", sizeof r);
        ui_row(1 + i, 40 + 24u * (uint32_t)i, 24, k < n ? br_name(g) : "", r,
               k < n && g == br_sel() ? K_TEAL : K_TEXT, k == br_cur ? K_CORAL : K_DIM,
               k == br_cur ? K_PANEL : K_BG);
    }
    ui_row(13, 214, 26, br_level == 3 ? "SEL play" : "SEL open", br_level ? "HOME up" : "HOME back", K_DIM, K_DIM,
           K_PANEL);
}

/* SEL in the browser: down a level, or tune the station and go home; 1 = back to Home */
static int br_enter(void)
{
    uint32_t g = br_first() + (uint32_t)br_cur;
    if (!br_count())
        return 0;
    if (br_level == 0) {
        sel_cont = (uint16_t)g;
        sel_country = rg_cont(g)->first;
    } else if (br_level == 1) {
        sel_country = (uint16_t)g;
        sel_city = rg_country(g)->first;
    } else if (br_level == 2) {
        sel_city = (uint16_t)g;
        sel_station = rg_city(g)->first;
    } else {
        sel_station = (uint16_t)g;
        tune_want = 1;
        tune_at = fm1_ms;
        if (!radio_want)
            radio_toggle();
        return 1;
    }
    br_level++;
    br_cur = 0;
    br_top = 0;
    return 0;
}

/* HOME in the browser: up a level (to the selection there); 1 = out, to Home */
static int br_up(void)
{
    if (!br_level)
        return 1;
    br_level--;
    br_cur = (int)(br_sel() - br_first());
    br_top = 0;
    return 0;
}

/* ---- held HOME: the controls */
static void ui_help(void)
{
    static const char *const L[8][2] = {
        {"KNOB 1..4", "continent .. station"}, {"SELECT", "next station"}, {"SEL", "browse the world"},
        {"PLAY", "play / stop"}, {"GLO", "WiFi, networks"}, {"MASTER", "volume"},
        {"keys / FX", "hold for FX / tap tempo"}, {"", ""}};
    int i;
    ui_title(0, "Controls", "");
    for (i = 0; i < 7; i++)
        ui_row(1 + i, 40 + 24u * (uint32_t)i, 24, L[i][0], L[i][1], K_TEXT, K_DIM, K_BG);
    ui_row(13, 214, 26, "let go of HOME", "", K_DIM, K_DIM, K_PANEL);
}

/* the wire's dump: the station selected, and whether the radio should play */
static void radio_ui_dump(void)
{
    char b[72];
    if (!sel_valid())
        return;
    snprintf(b, sizeof b, "station %u %s  want %u", sel_station, rg_str(rg_station(sel_station)->name), radio_want);
    wire_line(b);
}
