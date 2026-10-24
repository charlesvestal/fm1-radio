/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* The radio's pages, in Polly / X0X's grammar: HOME the station playing (KNOB 1..4 a world dial: continent,
 * country, place, station; SELECT steps the stations; it tunes a moment after the turning stops), SEL the
 * world as a list (SELECT moves, SEL opens, HOME goes up), PLAY plays / stops, hold HOME for the help, GLO
 * WiFi. The places come from the table in flash (rg.c); a place's stations from Radio Garden, fetched when
 * the place is chosen (radio_net.c rgn_*), and a station plays through Radio Garden's listen link.
 * Part of the app's unity build (app.c, after rg.c and the ui_* drawing). */

/* radio_net.c's Radio Garden side */
#define RGN_MAX 100u
typedef struct { char id[12], title[32]; } rgn_station_t;
enum { RGN_IDLE, RGN_BUSY, RGN_DONE, RGN_ERROR };
extern volatile int rgn_state;
extern rgn_station_t *rgn_list;
extern volatile uint32_t rgn_n;
extern char rgn_why[32];
extern int rgn_start(const char *place, const char *country);
extern int radio_net_tls(void);

/* ---- the selection */
static uint16_t sel_cont, sel_country, sel_place;   /* the continent and country (table indices), the place in it */
static char place_name[32];                       /* sel_place's name */
static int32_t sel_station = -1;                  /* in the place's list, when it is fetched; -1 none */
static char st_id[12], st_title[32];              /* the station chosen (kept, so it plays before a fetch) */
static char play_url[80];                         /* its listen link: radio_url points here */
static uint8_t radio_want;                        /* PLAY's: the radio should be on (kept across a power cycle) */
static int places_busy(void);                     /* places.c: the places being fetched (no stream meanwhile) */
enum { TW_NONE, TW_PLACE, TW_STATION };
static uint8_t tune_want;                         /* what to do once the turning has stopped */
static uint32_t tune_at;                          /* when (ms) */
/* the place's stations: fetched for (list_country, list_place); fetch_want asks, then_tune plays the first */
static uint8_t list_ok, fetch_want, fetch_busy, fetch_then_tune;
static int32_t step_pending;                      /* station-knob turns made before the place's list was here */
static uint16_t list_country;
static char list_place[32];
static uint32_t fetch_retry_at;

static int sel_valid(void) { return rg && rg->n_country; }
static int list_here(void) { return list_ok && list_country == sel_country && w_eq(list_place, place_name); }

static void place_load(void)                      /* place_name from sel_place */
{
    if (rg_place_name(sel_country, sel_place, place_name))
        w_cpy(place_name, "?", sizeof place_name);
}

/* a step d in [0, n), wrapping */
static uint16_t step_in(uint32_t cur, uint32_t n, int32_t d)
{
    int32_t i = (int32_t)cur + d;
    if (!n)
        return 0;
    i %= (int32_t)n;
    if (i < 0)
        i += (int32_t)n;
    return (uint16_t)i;
}

static void station_pick(int32_t i)               /* a station of the fetched list as the one to play */
{
    if (!list_here() || i < 0 || (uint32_t)i >= rgn_n)
        return;
    sel_station = i;
    w_cpy(st_id, rgn_list[i].id, sizeof st_id);
    w_cpy(st_title, rgn_list[i].title, sizeof st_title);
}

/* a knob: level 0 continent .. 3 station; the levels below go to their first */
static void sel_turn(int level, int32_t d)
{
    if (!sel_valid() || !d)
        return;
    if (level == 0) {
        sel_cont = step_in(sel_cont, rg->n_cont, d);
        sel_country = rg_cont(sel_cont)->first;
    } else if (level == 1) {
        const rg_cont_t *c = rg_cont(sel_cont);
        sel_country = (uint16_t)(c->first + step_in(sel_country - c->first, c->n, d));
    }
    if (level <= 1)
        sel_place = 0;
    else if (level == 2)
        sel_place = step_in(sel_place, rg_country(sel_country)->n, d);
    if (level <= 2) {                             /* a new place: its stations are fetched once the dial rests */
        place_load();
        sel_station = -1;
        st_title[0] = 0;
        step_pending = 0;
        tune_want = TW_PLACE;
    } else if (list_here() && rgn_n) {            /* a station of the list */
        station_pick(step_in(sel_station < 0 ? 0 : (uint32_t)sel_station, rgn_n, sel_station < 0 && d > 0 ? d - 1 : d));
        tune_want = TW_STATION;
    } else if (st_id[0]) {                        /* a station playing, its list not here (the start's restore,
                                                   * a favorite): fetched, then a step on from the station playing */
        step_pending += d;
        if (!fetch_busy) {
            fetch_want = 1;
            fetch_then_tune = 0;
            fetch_retry_at = plat_ms();
        }
        return;
    } else {                                      /* the list is not here yet: fetch it, then play its first */
        tune_want = TW_PLACE;
    }
    tune_at = plat_ms() + 900u;                      /* a dial: it tunes when the turning stops */
}

/* kept in the app's own object (OBJ_RADIO: app.c rset) */
static void radio_remember(void)
{
    if (rset.playing != radio_want || rset.country != sel_country || !w_eq(rset.place, place_name) ||
        !w_eq(rset.st_id, st_id)) {
        rset.playing = radio_want;
        rset.country = sel_country;
        w_cpy(rset.place, place_name, sizeof rset.place);
        w_cpy(rset.st_id, st_id, sizeof rset.st_id);
        w_cpy(rset.st_title, st_title, sizeof rset.st_title);
        rset_save();
    }
}

static void station_tune(void)                    /* st_id as the stream to play */
{
    if (radio_st != RS_OFF)
        radio_stop();
    if (st_id[0])
        snprintf(play_url, sizeof play_url, "https://radio.garden/api/ara/content/listen/%s/channel.mp3", st_id);
    else
        play_url[0] = 0;
    radio_url = play_url;
    radio_unplayable = 0;
    radio_remember();
}

/* ---- favorites (app/fav.c): the current station as one, and one tuned as the start restores the last */
static int fav_can_add(void)
{
    return fav && sel_valid() && st_id[0] && fav_find(fav, st_id) < 0 && fav->n < FAV_MAX;
}
static void fav_entry(fav_t *e)
{
    memset(e, 0, sizeof *e);
    e->country = sel_country;
    w_cpy(e->place, place_name, sizeof e->place);
    w_cpy(e->st_id, st_id, sizeof e->st_id);
    w_cpy(e->title, st_title, sizeof e->title);
}
static int fav_tune(const fav_t *e)               /* 0 tuned; -1 its place is gone from the table */
{
    int32_t p;
    if (!sel_valid() || e->country >= rg->n_country || (p = rg_place_index(e->country, e->place)) < 0)
        return -1;
    sel_country = e->country;
    sel_cont = rg_country(sel_country)->cont;
    sel_place = (uint16_t)p;
    place_load();
    w_cpy(st_id, e->st_id, sizeof st_id);
    w_cpy(st_title, e->title, sizeof st_title);
    tune_want = TW_NONE;
    radio_want = 1;
    station_tune();
    fetch_want = 1;                               /* its place's list, once the stream is going (as at the start) */
    fetch_then_tune = 0;
    fetch_retry_at = plat_ms() + 5000u;                               /* the old stream stopped; this one starts once the network is
                                                   * free (radio_ui_service), and is remembered as the last */
    return 0;
}

/* the main loop's: tune, fetch, start and stop as asked, once the network side is free */
static void radio_ui_service(void)
{
    if (!sel_valid())
        return;
    if (tune_want && (int32_t)(plat_ms() - tune_at) >= 0) {
        if (tune_want == TW_STATION) {
            station_tune();
        } else {                                  /* a place: its stations first */
            fetch_want = 1;
            fetch_then_tune = 1;
            fetch_retry_at = plat_ms();
        }
        tune_want = TW_NONE;
    }
    /* the place's stations, asked of Radio Garden: an https stream gives the TLS state up first */
    if (fetch_want && !fetch_busy && wifi_st == WS_CONNECTED && (int32_t)(plat_ms() - fetch_retry_at) >= 0) {
        if (radio_net_tls() && !fetch_then_tune && !step_pending) {
            fetch_retry_at = plat_ms() + 30000u;  /* a list for Home's count only: not worth stopping an https stream */
        } else if (radio_net_tls() || (radio_net_running() && radio_url == play_url && play_url[0] && fetch_then_tune)) {
            if (radio_st != RS_OFF)
                radio_stop();                     /* a new place anyway, or an https stream in the way */
        } else if (rgn_start(place_name, rg_str(rg_country(sel_country)->name)) == 0) {
            fetch_busy = 1;
            list_ok = 0;
            list_country = sel_country;
            w_cpy(list_place, place_name, sizeof list_place);
        } else {
            fetch_retry_at = plat_ms() + 1000u;
        }
    }
    if (fetch_busy && rgn_state != RGN_BUSY) {
        fetch_busy = 0;
        if (rgn_state == RGN_DONE) {
            fetch_want = 0;
            list_ok = 1;
            if (list_here()) {                    /* the station playing, found in it (Home's "3 of 22") */
                uint32_t i;
                sel_station = -1;
                for (i = 0; i < rgn_n; i++)
                    if (w_eq(rgn_list[i].id, st_id)) {
                        sel_station = (int32_t)i;
                        break;
                    }
                if (step_pending && rgn_n) {      /* the turns made meanwhile, from there */
                    station_pick(step_in(sel_station < 0 ? 0 : (uint32_t)sel_station, rgn_n,
                                         sel_station < 0 && step_pending > 0 ? step_pending - 1 : step_pending));
                    tune_want = TW_STATION;
                    tune_at = plat_ms();
                }
            }
            step_pending = 0;
            if (list_here() && fetch_then_tune) {
                fetch_then_tune = 0;
                if (rgn_n) {
                    station_pick(0);
                    station_tune();
                }
            }
        } else {
            fetch_retry_at = plat_ms() + 10000u;     /* no network, or Radio Garden: again in a while */
        }
    }
    if (radio_want && (radio_st == RS_OFF || (radio_st == RS_ERROR && !radio_unplayable)) && play_url[0] &&
        wifi_st == WS_CONNECTED && !radio_net_running() && !tune_want && !fetch_busy && !places_busy() &&
        !(fetch_want && fetch_then_tune)) {
        static uint32_t last_try;
        if (radio_st == RS_OFF || plat_ms() - last_try > 10000u) {   /* a stopped stream: try again now and then */
            last_try = plat_ms();
            radio_url = play_url;
            radio_play(0);
        }
    }
    if ((!radio_want || (wifi_st != WS_CONNECTED && radio_mode != 2)) && radio_st != RS_OFF)
        radio_stop();                             /* (WiFi gone: the stream's RAM back for the rejoin; it starts again
                                                   * once connected, as radio_want says) */
}

static void radio_toggle(void)
{
    radio_want = !radio_want;
    if (radio_want && wifi_st == WS_OFF)
        wifi_power(1);
    if (radio_want && !st_id[0] && sel_valid())
        tune_want = TW_PLACE, tune_at = plat_ms();   /* nothing chosen yet: this place's first station */
    radio_remember();
}

/* at boot: the last session's place and station */
static void radio_ui_restore(void)
{
    int32_t p;
    if (!sel_valid())
        return;
    sel_country = rset.country < rg->n_country ? rset.country : 0;
    sel_cont = rg_country(sel_country)->cont;
    p = rset.place[0] ? rg_place_index(sel_country, rset.place) : -1;
    sel_place = p >= 0 ? (uint16_t)p : 0;
    place_load();
    radio_want = rset.playing;
    if (p >= 0 && rset.st_id[0]) {                /* the station itself: it plays before the list is fetched */
        w_cpy(st_id, rset.st_id, sizeof st_id);
        w_cpy(st_title, rset.st_title, sizeof st_title);
        snprintf(play_url, sizeof play_url, "https://radio.garden/api/ara/content/listen/%s/channel.mp3", st_id);
        radio_url = play_url;
        fetch_want = 1;                           /* its place's list, once the stream is going: Home's "3 of 22" */
        fetch_then_tune = 0;
        fetch_retry_at = plat_ms() + 5000u;
    } else if (radio_want) {
        tune_want = TW_PLACE;
        tune_at = 0;
    }
}

/* ---- drawing */
static void ui_text_big(int slot, uint32_t y, const char *t, uint16_t c)   /* the station's name, as large as fits */
{
    char key[UI_KEY];
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
    char key[UI_KEY], v[24];
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
    uint16_t sc, lc = K_DIM;
    if (wifi_st == WS_CONNECTED)
        snprintf(hint, sizeof hint, "WiFi %d", wifi_rssi);
    else
        w_cpy(hint, wifi_st == WS_OFF ? "WiFi off" : wifi_st == WS_FAILED ? "no WiFi" : "WiFi ...", sizeof hint);
    ui_title(0, "Radio", hint);
    if (!sel_valid()) {                           /* no places yet: Radio Garden's, being fetched (places.c) */
        char st[48];
        ui_text_big(1, 42, "Getting the places", K_TEXT);
        ui_row(2, 74, 22, "from Radio Garden: only this once", "", K_DIM, K_DIM, K_BG);
        ui_row(3, 96, 22, places_status(st, sizeof st) ? st : "", "", K_TEAL, K_DIM, K_BG);
        ui_row(13, 214, 26, "GLO wifi", "", K_DIM, K_DIM, K_PANEL);
        return;
    }
    sc = radio_st == RS_PLAYING ? K_TEAL : radio_st == RS_ERROR ? K_CORAL : K_TEXT;
    ui_text_big(1, 42, st_title[0] ? st_title : fetch_busy || fetch_want ? "Finding stations" : "Choose a station",
                tune_want ? K_CORAL : st_title[0] ? sc : K_DIM);
    snprintf(line, sizeof line, "%s, %s", place_name, rg_str(rg_country(sel_country)->name));
    ui_row(2, 74, 22, line, "", K_DIM, K_DIM, K_BG);
    if (wifi_st != WS_CONNECTED)
        w_cpy(line, plat_wifi_saved(0, 0) ? "waiting for WiFi" : "no WiFi: GLO to join a network", sizeof line);
    else if (fetch_busy)
        w_cpy(line, "asking Radio Garden...", sizeof line);
    else if (fetch_want && rgn_state == RGN_ERROR)
        snprintf(line, sizeof line, "%s: trying again", rgn_why), lc = K_CORAL;
    else if (list_here() && !rgn_n)
        w_cpy(line, "no stations here", sizeof line), lc = K_CORAL;
    else if (!radio_want)
        w_cpy(line, "stopped", sizeof line);
    else if (radio_st == RS_PLAYING)
        snprintf(line, sizeof line, "playing  %u kbit/s", radio_kbps);
    else if (radio_st == RS_BUFFERING)
        snprintf(line, sizeof line, "buffering %u%%", (radio_wr - radio_rd) * 100u / NET_RING);
    else if (radio_st == RS_ERROR && radio_unplayable)
        snprintf(line, sizeof line, "%s: turn K4", radio_why), lc = K_CORAL;
    else if (radio_st == RS_ERROR)
        snprintf(line, sizeof line, "%s: trying again", radio_why), lc = K_CORAL;
    else
        w_cpy(line, "tuning...", sizeof line);
    if (!fx_held && (int32_t)(tempo_shown_until - plat_ms()) > 0) {   /* just tapped: the tempo */
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
        ui_row(3, 96, 22, line, "", lc, K_DIM, K_BG);
    }
    {
        char v[24];
        ui_cell(4, 8, 124, "K1", "Continent", rg_str(rg_cont(sel_cont)->name), 0);
        ui_cell(5, 120, 124, "K2", "Country", rg_str(rg_country(sel_country)->name), 0);
        ui_cell(6, 8, 168, "K3", "Place", place_name, 0);
        if (list_here() && sel_station >= 0)
            snprintf(v, sizeof v, "%u of %u", (unsigned)(sel_station + 1), (unsigned)rgn_n);
        else
            w_cpy(v, list_here() ? "-" : "...", sizeof v);
        ui_cell(7, 120, 168, "K4", "Station", v, 0);
    }
    ui_row(13, 214, 26, radio_want ? "PLAY stop  SEL browse" : "PLAY play  SEL browse", "GLO wifi", K_DIM, K_DIM,
           K_PANEL);
}

/* ---- the browser: a level of the world as a list */
static uint8_t br_level;                          /* 0 continents, 1 countries, 2 places, 3 stations */
static int br_cur, br_top;
static char br_rows[7][32];                       /* level 2: the visible places' names */
static int32_t br_rows_top = -1;
static uint16_t br_rows_country;

static uint32_t br_count(void)
{
    return br_level == 0 ? rg->n_cont : br_level == 1 ? rg_cont(sel_cont)->n
         : br_level == 2 ? rg_country(sel_country)->n : list_here() ? rgn_n : 0u;
}
static int br_sel(void)                           /* the selection at this level, as a row */
{
    return br_level == 0 ? sel_cont : br_level == 1 ? sel_country - rg_cont(sel_cont)->first
         : br_level == 2 ? sel_place : sel_station;
}
static void br_row_name(uint32_t i, const char *s, void *ctx)
{
    (void)ctx;
    if (i >= (uint32_t)br_rows_top && i < (uint32_t)br_rows_top + 7u)
        w_cpy(br_rows[i - (uint32_t)br_rows_top], s, sizeof br_rows[0]);
}
static const char *br_name(uint32_t k)
{
    if (br_level == 0)
        return rg_str(rg_cont(k)->name);
    if (br_level == 1)
        return rg_str(rg_country(rg_cont(sel_cont)->first + k)->name);
    if (br_level == 2) {
        if (br_rows_top != br_top || br_rows_country != sel_country) {   /* the visible rows, unpacked once */
            memset(br_rows, 0, sizeof br_rows);
            br_rows_top = br_top;
            br_rows_country = sel_country;
            rg_names(sel_country, (uint32_t)br_top, 7, br_row_name, 0);
        }
        return k >= (uint32_t)br_top && k < (uint32_t)br_top + 7u ? br_rows[k - (uint32_t)br_top] : "";
    }
    return rgn_list[k].title;
}

static void br_open(void)                         /* the browser at the place's stations, at the station */
{
    br_level = list_here() ? 3 : 2;
    br_cur = br_level == 3 ? (sel_station < 0 ? 0 : sel_station) : sel_place;
    br_top = 0;
}

static void ui_browse(void)
{
    char title[40];
    int i, n;
    if (br_level == 3 && !list_here() && !fetch_busy) {   /* a place's stations: asked for when opened */
        fetch_want = 1;
        fetch_then_tune = 0;
        fetch_retry_at = plat_ms();
    }
    n = (int)br_count();
    w_cpy(title, br_level == 0 ? "World" : br_level == 1 ? rg_str(rg_cont(sel_cont)->name) :
          br_level == 2 ? rg_str(rg_country(sel_country)->name) : place_name, sizeof title);
    ui_title(0, title, br_level == 3 && !list_here() ? "loading..." : "");
    if (br_cur >= n)
        br_cur = n ? n - 1 : 0;
    if (br_cur < br_top)
        br_top = br_cur;
    if (br_cur >= br_top + 7)
        br_top = br_cur - 6;
    for (i = 0; i < 7; i++) {
        int k = br_top + i;
        char r[16];
        r[0] = 0;
        if (k < n && br_level == 0)
            snprintf(r, sizeof r, "%u", (unsigned)rg_cont((uint32_t)k)->n);
        else if (k < n && br_level == 1)
            snprintf(r, sizeof r, "%u", (unsigned)rg_country(rg_cont(sel_cont)->first + (uint32_t)k)->n);
        else if (k < n && br_level == 3 && k == sel_station && w_eq(rgn_list[k].id, st_id) && radio_st == RS_PLAYING)
            w_cpy(r, "playing", sizeof r);
        ui_row(1 + i, 40 + 24u * (uint32_t)i, 24, k < n ? br_name((uint32_t)k) : "", r,
               k < n && k == br_sel() ? K_TEAL : K_TEXT, k == br_cur ? K_CORAL : K_DIM,
               k == br_cur ? K_PANEL : K_BG);
    }
    ui_row(13, 214, 26, br_level == 3 ? "SEL play" : "SEL open", br_level ? "HOME up" : "HOME back", K_DIM, K_DIM,
           K_PANEL);
}

/* SEL in the browser: down a level, or tune the station and go home; 1 = back to Home */
static int br_enter(void)
{
    if (!br_count())
        return 0;
    if (br_level == 0) {
        sel_cont = (uint16_t)br_cur;
        sel_country = rg_cont(sel_cont)->first;
        sel_place = 0;
        place_load();
    } else if (br_level == 1) {
        sel_country = (uint16_t)(rg_cont(sel_cont)->first + (uint32_t)br_cur);
        sel_place = 0;
        place_load();
    } else if (br_level == 2) {
        if (sel_place != (uint16_t)br_cur || !list_here()) {
            sel_place = (uint16_t)br_cur;
            place_load();
            sel_station = -1;
            fetch_want = 1;                       /* its stations, to choose from */
            fetch_then_tune = 0;
            fetch_retry_at = plat_ms();
        }
    } else {
        station_pick(br_cur);
        tune_want = TW_STATION;
        tune_at = plat_ms();
        if (!radio_want)
            radio_toggle();
        return 1;
    }
    br_level++;
    br_cur = br_level == 3 ? (sel_station < 0 ? 0 : sel_station) : 0;
    br_top = 0;
    return 0;
}

/* HOME in the browser: up a level (to the selection there); 1 = out, to Home */
static int br_up(void)
{
    if (!br_level)
        return 1;
    br_level--;
    br_cur = br_sel() < 0 ? 0 : br_sel();
    br_top = 0;
    return 0;
}

/* ---- held HOME: the controls */
static void ui_help(void)
{
    static const char *const L[9][2] = {
        {"KNOB 1..4", "continent .. station"}, {"SELECT", "next station"}, {"SEL", "browse the world"},
        {"PLAY", "play / stop"}, {"SAVE", "favorites"}, {"ARP", "synth on the keys"}, {"GLO", "WiFi, networks"},
        {"MASTER", "volume"}, {"keys / FX", "hold for FX / tap tempo"}};
    int i;
    ui_title(0, "Controls", "");
    for (i = 0; i < 9; i++)
        ui_row(1 + i, 40 + 19u * (uint32_t)i, 19, L[i][0], L[i][1], K_TEXT, K_DIM, K_BG);
    ui_row(13, 214, 26, "let go of HOME", "", K_DIM, K_DIM, K_PANEL);
}

/* the wire's dump: the place and station, and whether the radio should play */
static void radio_ui_dump(void)
{
    char b[96];
    if (!sel_valid())
        return;
    snprintf(b, sizeof b, "place %s, %s  station %s %s  want %u", place_name, rg_str(rg_country(sel_country)->name),
             st_id, st_title, radio_want);
    wire_line(b);
    snprintf(b, sizeof b, "list %s %u  fetch %u/%u state %d %s", list_here() ? "here" : "-", (unsigned)rgn_n,
             fetch_want, fetch_busy, rgn_state, rgn_why);
    wire_line(b);
    if (fav) {
        snprintf(b, sizeof b, "fav %u sel %d", (unsigned)fav->n, fav_sel);
        wire_line(b);
    }
}
