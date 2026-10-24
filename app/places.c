/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Radio Garden's places, fetched by the FM-1 itself into the flash's data area (fm1-base plat_data): nothing of
 * Radio Garden's is in the firmware. The list (net/radio_net.c rgp_*, about 1.9 MB, the places in no order) is read
 * once to count each country's places, then again for each batch of countries that fits a RAM buffer: their names
 * sorted, front coded, and written one after another as the table rg.c reads (tools/rg_places.py's layout, its
 * blocks stored rather than deflated). The header goes last, with the CRC: a table cut short is never read.
 * Part of the app's unity build (app.c, after rg.c). */

/* the continents and their countries, as Radio Garden names them (folded to ASCII); one not here goes to "Other"
 * (tools/rg_places.py CONTINENT, after schwung-radiogarden's browser) */
static const char CONTS[] =
    "Africa\0"
        "Algeria\0"
        "Angola\0"
        "Benin\0"
        "Botswana\0"
        "Burkina Faso\0"
        "Burundi\0"
        "Cameroon\0"
        "Cape Verde\0"
        "Central African Republic\0"
        "Chad\0"
        "Comoros\0"
        "Cote d'Ivoire\0"
        "Democratic Republic of the Cong\0"
        "Djibouti\0"
        "Egypt\0"
        "Equatorial Guinea\0"
        "Eritrea\0"
        "Eswatini\0"
        "Ethiopia\0"
        "Gabon\0"
        "Ghana\0"
        "Guine-Bissau\0"
        "Guinea\0"
        "Kenya\0"
        "Lesotho\0"
        "Liberia\0"
        "Libya\0"
        "Madagascar\0"
        "Malawi\0"
        "Mali\0"
        "Mauritania\0"
        "Mauritius\0"
        "Mayotte\0"
        "Morocco\0"
        "Mozambique\0"
        "Namibia\0"
        "Niger\0"
        "Nigeria\0"
        "Republic of the Congo\0"
        "Reunion\0"
        "Rwanda\0"
        "Saint Helena\0"
        "Sao Tome and Principe\0"
        "Senegal\0"
        "Seychelles\0"
        "Sierra Leone\0"
        "Somalia\0"
        "South Africa\0"
        "South Sudan\0"
        "Sudan\0"
        "Tanzania\0"
        "The Gambia\0"
        "Togo\0"
        "Tunisia\0"
        "Uganda\0"
        "Western Sahara\0"
        "Zambia\0"
        "Zimbabwe\0"
        "\0"
    "Asia\0"
        "Afghanistan\0"
        "Armenia\0"
        "Azerbaijan\0"
        "Bahrain\0"
        "Bangladesh\0"
        "Bhutan\0"
        "Brunei\0"
        "Cambodia\0"
        "China\0"
        "Cyprus\0"
        "Georgia\0"
        "Hong Kong\0"
        "India\0"
        "Indonesia\0"
        "Iran\0"
        "Iraq\0"
        "Israel\0"
        "Japan\0"
        "Jordan\0"
        "Kazakhstan\0"
        "Kuwait\0"
        "Kyrgyzstan\0"
        "Laos\0"
        "Lebanon\0"
        "Macau\0"
        "Malaysia\0"
        "Maldives\0"
        "Mongolia\0"
        "Myanmar (Burma)\0"
        "Nepal\0"
        "North Korea\0"
        "Oman\0"
        "Pakistan\0"
        "Palestine\0"
        "Philippines\0"
        "Qatar\0"
        "Saudi Arabia\0"
        "Singapore\0"
        "South Korea\0"
        "Sri Lanka\0"
        "Syria\0"
        "Taiwan\0"
        "Tajikistan\0"
        "Thailand\0"
        "Timor-Leste\0"
        "Turkiye\0"
        "Turkmenistan\0"
        "United Arab Emirates\0"
        "Uzbekistan\0"
        "Vietnam\0"
        "Yemen\0"
        "\0"
    "Europe\0"
        "Aland Islands\0"
        "Albania\0"
        "Andorra\0"
        "Austria\0"
        "Azores\0"
        "Bailiwick of Guernsey\0"
        "Bailiwick of Jersey\0"
        "Belarus\0"
        "Belgium\0"
        "Bosnia and Herzegovina\0"
        "Bulgaria\0"
        "Croatia\0"
        "Czechia\0"
        "Denmark\0"
        "Estonia\0"
        "Faroe Islands\0"
        "Finland\0"
        "France\0"
        "Germany\0"
        "Gibraltar\0"
        "Greece\0"
        "Hungary\0"
        "Iceland\0"
        "Ireland\0"
        "Isle of Man\0"
        "Italy\0"
        "Kosovo\0"
        "Latvia\0"
        "Liechtenstein\0"
        "Lithuania\0"
        "Luxembourg\0"
        "Madeira\0"
        "Malta\0"
        "Moldova\0"
        "Monaco\0"
        "Montenegro\0"
        "Netherlands\0"
        "North Macedonia\0"
        "Norway\0"
        "Poland\0"
        "Portugal\0"
        "Romania\0"
        "Russia\0"
        "San Marino\0"
        "Serbia\0"
        "Slovakia\0"
        "Slovenia\0"
        "Spain\0"
        "Sweden\0"
        "Switzerland\0"
        "Ukraine\0"
        "United Kingdom\0"
        "Vatican City\0"
        "\0"
    "N. America\0"
        "American Samoa\0"
        "Anguilla\0"
        "Antigua and Barbuda\0"
        "Aruba\0"
        "Barbados\0"
        "Belize\0"
        "Bermuda\0"
        "Bonaire\0"
        "British Virgin Islands\0"
        "Canada\0"
        "Cayman Islands\0"
        "Collectivity of Saint Martin\0"
        "Costa Rica\0"
        "Cuba\0"
        "Curacao\0"
        "Dominica\0"
        "Dominican Republic\0"
        "El Salvador\0"
        "Greenland\0"
        "Grenada\0"
        "Guadeloupe\0"
        "Guatemala\0"
        "Haiti\0"
        "Honduras\0"
        "Jamaica\0"
        "Martinique\0"
        "Mexico\0"
        "Montserrat\0"
        "Nicaragua\0"
        "Panama\0"
        "Puerto Rico\0"
        "Saint Barthelemy\0"
        "Saint Kitts and Nevis\0"
        "Saint Lucia\0"
        "Saint Vincent and the Grenadine\0"
        "Saint-Pierre et Miquelon\0"
        "Sint Maarten\0"
        "The Bahamas\0"
        "Trinidad and Tobago\0"
        "Turks and Caicos Islands\0"
        "U.S. Virgin Islands\0"
        "United States\0"
        "\0"
    "Oceania\0"
        "Australia\0"
        "Cook Islands\0"
        "Federated States of Micronesia\0"
        "Fiji\0"
        "Guam\0"
        "Kiribati\0"
        "Marshall Islands\0"
        "Nauru\0"
        "New Caledonia\0"
        "New Zealand\0"
        "Niue\0"
        "Northern Mariana Islands\0"
        "Palau\0"
        "Papua New Guinea\0"
        "Samoa\0"
        "Solomon Islands\0"
        "Tahiti\0"
        "Tonga\0"
        "Tuvalu\0"
        "Vanuatu\0"
        "Wallis and Futuna\0"
        "\0"
    "S. America\0"
        "Argentina\0"
        "Bolivia\0"
        "Brazil\0"
        "Chile\0"
        "Colombia\0"
        "Ecuador\0"
        "Falkland Islands\0"
        "French Guiana\0"
        "Guyana\0"
        "Paraguay\0"
        "Peru\0"
        "Suriname\0"
        "Uruguay\0"
        "Venezuela\0"
        "\0";
#define RGP_OFF PLAT_DATA_LO                       /* the table: 0x97000 .. */
#define RGP_END 0xD6000u                           /* .. OBJ_FAV's sectors (app.h): 258 KB for a table of ~130 */
#define PC_MAX 250u
#define CONT_MAX 8u

typedef struct {
    char name[32];
    uint32_t bytes, block, len;                    /* its names' bytes (with NULs); where its block went */
    uint16_t n, kept;                              /* places in the list; kept (once each) */
    uint8_t cont, batch;
} pc_t;
enum { PB_IDLE, PB_COUNT, PB_ERASE, PB_BATCH, PB_WRITE, PB_FAILED };
static struct {
    uint8_t state, pass, passes, overflow, tries;   /* tries: a pass started again after a dropped download */
    uint16_t n_pc, n_cont, cur;                    /* cur: the next country to write (in order) */
    uint32_t retry_at, erase_at, erase_end, w, off_blocks, places;
    uint32_t list_kb;                              /* the list's size (the count's pass), for the time left */
    char why[32];
} pb;
static pc_t *pc;                                   /* PC_MAX, from the heap while building */
static uint16_t *pc_order;                         /* the countries in the table's order */
static char cont_name[CONT_MAX][12];
static uint8_t *bbuf;                              /* a batch's names: [country, name, NUL]... */
static uint32_t bbuf_size, bbuf_used;
enum { RGP_IDLE, RGP_BUSY, RGP_DONE, RGP_ERROR };      /* net/radio_net.c's */
extern volatile int rgp_state;
extern volatile uint32_t rgp_bytes;
extern char rgp_why[32];
extern int rgp_start(void (*cb)(const char *title, const char *country));

/* a name as the table keeps it: spaces collapsed, trimmed, at most 31 characters (rg_places.py ascii_name) */
static void pl_clean(char *d, const char *s)
{
    unsigned n = 0;
    while (*s == ' ')
        s++;
    for (; *s && n < 31u; s++)
        if (*s != ' ' || (s[1] && s[1] != ' '))
            d[n++] = *s;
    while (n && d[n - 1] == ' ')
        n--;
    d[n] = 0;
}
static uint8_t pl_continent(const char *country)
{
    const char *p = CONTS;
    uint8_t ci = 0;
    while (*p) {                                   /* a continent: its name, its countries, an empty string */
        p += strlen(p) + 1;
        while (*p) {
            if (!strcmp(p, country))
                return ci;
            p += strlen(p) + 1;
        }
        p++;
        ci++;
    }
    return ci;                                     /* "Other" */
}

/* the list's callback, in the fetch's task: counts (pass 0) or copies a batch's names */
static void pl_place(const char *title, const char *country)
{
    char t[32], c[32];
    uint16_t i;
    pl_clean(t, title);
    pl_clean(c, country);
    if (!t[0] || !c[0])
        return;
    for (i = 0; i < pb.n_pc && strcmp(pc[i].name, c); i++)
        ;
    if (pb.pass == 0) {
        if (i == pb.n_pc) {
            if (pb.n_pc >= PC_MAX)
                return;
            memset(&pc[i], 0, sizeof pc[i]);
            memcpy(pc[i].name, c, sizeof c);
            pc[i].cont = pl_continent(c);
            pb.n_pc++;
        }
        pc[i].n++;
        pc[i].bytes += (uint32_t)strlen(t) + 1u;
    } else if (i < pb.n_pc && pc[i].batch == pb.pass) {
        uint32_t n = (uint32_t)strlen(t) + 2u;
        if (bbuf_used + n > bbuf_size) {
            pb.overflow = 1;
            return;
        }
        bbuf[bbuf_used] = (uint8_t)i;
        memcpy(bbuf + bbuf_used + 1u, t, n - 1u);
        bbuf_used += n;
    }
}

static int pl_lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }
static int pl_cmp(const char *a, const char *b)    /* rg_places.py's order: without case, then as is */
{
    const char *x = a, *y = b;
    for (; *x && pl_lower(*x) == pl_lower(*y); x++, y++)
        ;
    if (pl_lower(*x) != pl_lower(*y))
        return pl_lower(*x) - pl_lower(*y);
    return strcmp(a, b);
}

#define PL_NET_ROOM (16709u + 2048u + 1024u)     /* radio_net.c: a TLS record buffer, a js_t, and some over */
static void pl_free(void)
{
    free(pc);
    free(pc_order);
    arena_free(bbuf);
    pc = 0;
    pc_order = 0;
    bbuf = 0;
}
/* a missing table comes first: a playing stream is stopped for the fetch (one TLS connection, and the RAM, at a time)
 * and comes back after it (radio_ui_restore). Not after a failure: a fetch that keeps failing waits for the stream to
 * end, or every retry would cut it off. A refresh asks again. */
static uint8_t pl_first = 1;
static void pl_fail(const char *why)
{
    w_cpy(pb.why, why, sizeof pb.why);
    plat_log(pb.why);
    pl_free();
    pb.state = PB_FAILED;
    pb.retry_at = plat_ms() + 30000u;
    pl_first = 0;
}
/* radio_ui's: no stream while the places are being fetched, or are about to be */
static int places_busy(void)
{
    return (pb.state != PB_IDLE && pb.state != PB_FAILED) || (pb.state == PB_IDLE && !rg && pl_first);
}

/* the table's strings: the continents', then the countries' */
static uint32_t pl_strings_size(void)
{
    uint32_t n = 0, i;
    for (i = 0; i < pb.n_cont; i++)
        n += (uint32_t)strlen(cont_name[i]) + 1u;
    for (i = 0; i < pb.n_pc; i++)
        n += (uint32_t)strlen(pc[i].name) + 1u;
    return n;
}

/* after the count: the order (continents, their countries by name), the batches, where the blocks start */
static int pl_plan(void)
{
    uint32_t i, j, k = 0, cum = 0, need = 0, biggest = 0, total;
    const char *p = CONTS;
    pb.n_cont = 0;
    while (*p && pb.n_cont < CONT_MAX - 1u) {     /* the continents' names */
        w_cpy(cont_name[pb.n_cont++], p, sizeof cont_name[0]);
        p += strlen(p) + 1;
        while (*p)
            p += strlen(p) + 1;
        p++;
    }
    w_cpy(cont_name[pb.n_cont++], "Other", sizeof cont_name[0]);
    {   /* only the continents with countries: the others' places are closed up */
        uint8_t map[CONT_MAX], n = 0;
        for (i = 0; i < pb.n_cont; i++) {
            for (j = 0; j < pb.n_pc && pc[j].cont != i; j++)
                ;
            map[i] = n;
            if (j < pb.n_pc) {
                if (n != i)
                    memcpy(cont_name[n], cont_name[i], sizeof cont_name[0]);
                n++;
            }
        }
        for (j = 0; j < pb.n_pc; j++)
            pc[j].cont = map[pc[j].cont];
        pb.n_cont = n;
    }
    pc_order = malloc(pb.n_pc * sizeof *pc_order);
    if (!pc_order)
        return -1;
    for (i = 0; i < pb.n_cont; i++)               /* by continent, then by name */
        for (j = 0; j < pb.n_pc; j++)
            if (pc[j].cont == i) {
                uint32_t m = k++;
                while (m && pc[pc_order[m - 1]].cont == i && pl_cmp(pc[pc_order[m - 1]].name, pc[j].name) > 0) {
                    pc_order[m] = pc_order[m - 1];
                    m--;
                }
                pc_order[m] = (uint16_t)j;
            }
    for (i = 0; i < pb.n_pc; i++) {
        need += pc[i].bytes + pc[i].n + 5u;       /* (the batch: a country byte each; the block: its header) */
        if (pc[i].bytes + pc[i].n > biggest)
            biggest = pc[i].bytes + pc[i].n;
    }
    {   /* the batches download again: leave room for that connection (its JSON reader, then its TLS record
         * buffer, BR_SSL_BUFSIZE_MONO 16709 bytes), or the largest buffer takes it ("tls: no memory") */
        void *room;
        if ((bbuf = arena_take(2, 72u * 1024u)) != 0)   /* the big block kept for this (radio.c) */
            bbuf_size = arena_n;
        room = bbuf ? 0 : malloc(PL_NET_ROOM);
        if (!bbuf)
            bbuf_size = 72u * 1024u;
        for (; !bbuf && bbuf_size >= biggest; bbuf_size -= 4096u)
            if ((bbuf = malloc(bbuf_size)) != 0)
                break;
        free(room);
    }
    if (!bbuf)
        return -2;
    pb.passes = 1;
    for (i = 0; i < pb.n_pc; i++) {               /* batches, in the table's order */
        pc_t *c = &pc[pc_order[i]];
        if (cum + c->bytes + c->n > bbuf_size) {
            pb.passes++;
            cum = 0;
        }
        cum += c->bytes + c->n;
        c->batch = pb.passes;
    }
    pb.off_blocks = (64u + 8u * pb.n_cont + 16u * pb.n_pc + pl_strings_size() + 3u) & ~3u;
    total = pb.off_blocks + need;
    if (RGP_OFF + total > RGP_END)
        return -3;
    pb.erase_at = RGP_OFF;
    pb.erase_end = RGP_OFF + ((total + 0xFFFu) & ~0xFFFu);
    pb.w = RGP_OFF + pb.off_blocks;
    return 0;
}

static int pl_write(const void *p, uint32_t n)    /* at pb.w, from RAM */
{
    if (pb.w + n > RGP_END || plat_data_write(pb.w, p, n))
        return -1;
    pb.w += n;
    return 0;
}

/* a batch's countries: each one's names sorted, once each, front coded into a stored deflate block */
static int pl_country(uint16_t ci)
{
    static uint8_t out[256];
    pc_t *c = &pc[ci];
    const char **v = malloc((c->n ? c->n : 1u) * sizeof *v), *prev = "";
    uint32_t i = 0, k, n = 0, o = 0, len = 0, start;
    uint8_t hdr[5];
    if (!v)
        return -1;
    while (i < bbuf_used) {                        /* its names, from the batch */
        const char *s = (const char *)bbuf + i + 1;
        uint32_t l = (uint32_t)strlen(s);
        if (bbuf[i] == ci && n < c->n) {
            uint32_t m = n++;                      /* insertion into order (a country's few hundred) */
            while (m && pl_cmp(v[m - 1], s) > 0) {
                v[m] = v[m - 1];
                m--;
            }
            v[m] = s;
        }
        i += l + 2u;
    }
    for (i = 0; i < n; i++)                        /* the block's length first: its header says it */
        if (!i || strcmp(v[i], v[i - 1])) {
            const char *a = v[i];
            for (k = 0; k < 255u && a[k] && a[k] == prev[k]; k++)
                ;
            len += 1u + (uint32_t)strlen(a + k) + 1u;
            prev = a;
        }
    start = pb.w;
    hdr[0] = 1;                                    /* BFINAL, stored */
    hdr[1] = (uint8_t)len;
    hdr[2] = (uint8_t)(len >> 8);
    hdr[3] = (uint8_t)~len;
    hdr[4] = (uint8_t)(~len >> 8);
    if (len > 0xFFFFu || pl_write(hdr, 5)) {
        free(v);
        return -1;
    }
    prev = "";
    c->kept = 0;
    for (i = 0; i < n; i++) {
        const char *a = v[i];
        uint32_t l;
        if (i && !strcmp(a, v[i - 1]))
            continue;
        for (k = 0; k < 255u && a[k] && a[k] == prev[k]; k++)
            ;
        l = (uint32_t)strlen(a + k);
        if (o + l + 2u > sizeof out) {
            if (pl_write(out, o)) {
                free(v);
                return -1;
            }
            o = 0;
        }
        out[o++] = (uint8_t)k;
        memcpy(out + o, a + k, l + 1u);
        o += l + 1u;
        prev = a;
        c->kept++;
    }
    free(v);
    if (o && pl_write(out, o))
        return -1;
    c->block = start - RGP_OFF;
    c->len = pb.w - start;
    pb.places += c->kept;
    return 0;
}

static uint32_t pl_crc(const uint8_t *p, uint32_t n)   /* CRC-32 (zlib's) */
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* the tables, the strings, then the header: the table is whole */
static int pl_tables(void)
{
    uint8_t row[16];
    uint32_t i, j, s = 0, total = pb.w - RGP_OFF, str_at = 64u + 8u * pb.n_cont + 16u * pb.n_pc;
    uint32_t cont_str[CONT_MAX], first = 0;
    rg_hdr_t h;
    pb.w = RGP_OFF + 64u;
    for (i = 0; i < pb.n_cont; i++) {              /* continents: name, first country, countries */
        uint32_t cnt = 0;
        for (j = 0; j < pb.n_pc; j++)
            cnt += pc[pc_order[j]].cont == i;
        cont_str[i] = s;
        s += (uint32_t)strlen(cont_name[i]) + 1u;
        memcpy(row, &cont_str[i], 4);
        row[4] = (uint8_t)first;
        row[5] = (uint8_t)(first >> 8);
        row[6] = (uint8_t)cnt;
        row[7] = (uint8_t)(cnt >> 8);
        first += cnt;
        if (pl_write(row, 8))
            return -1;
    }
    for (j = 0; j < pb.n_pc; j++) {                /* countries: name, block, length, places, continent */
        pc_t *c = &pc[pc_order[j]];
        uint32_t blk = c->block;
        memcpy(row, &s, 4);
        s += (uint32_t)strlen(c->name) + 1u;
        memcpy(row + 4, &blk, 4);
        memcpy(row + 8, &c->len, 4);
        row[12] = (uint8_t)c->kept;
        row[13] = (uint8_t)(c->kept >> 8);
        row[14] = c->cont;
        row[15] = 0;
        if (pl_write(row, 16))
            return -1;
    }
    for (i = 0; i < pb.n_cont; i++)
        if (pl_write(cont_name[i], (uint32_t)strlen(cont_name[i]) + 1u))
            return -1;
    for (j = 0; j < pb.n_pc; j++)
        if (pl_write(pc[pc_order[j]].name, (uint32_t)strlen(pc[pc_order[j]].name) + 1u))
            return -1;
    memset(&h, 0, sizeof h);
    h.magic = RG_MAGIC;
    h.version = 1;
    h.n_cont = pb.n_cont;
    h.n_country = pb.n_pc;
    h.n_place = (uint16_t)pb.places;
    h.off_cont = 64;
    h.off_country = 64u + 8u * pb.n_cont;
    h.off_str = str_at;
    h.off_blocks = pb.off_blocks;
    h.total = total;
    h.crc = pl_crc(plat_data(RGP_OFF) + 64, total - 64u);
    pb.w = RGP_OFF;
    return pl_write(&h, sizeof h);
}

/* the main loop's: start when there is no table and WiFi is up; one step at a time */
static void places_poll(void)
{
    switch (pb.state) {
    case PB_FAILED:
        if ((int32_t)(plat_ms() - pb.retry_at) < 0)
            break;
        pb.state = PB_IDLE;
        /* fall through */
    case PB_IDLE:
        if (rg || wifi_st != WS_CONNECTED)
            break;
        if (radio_net_running()) {                 /* a stream: stopped first (pl_first), its connection let go */
            if (pl_first && radio_st != RS_OFF)
                radio_stop();
            break;
        }
        if (radio_st != RS_OFF)
            radio_stop();                          /* (one TLS connection at a time) */
        dec_drop();                                /* and the decoder's RAM back (AAC's 53 KB, which a stop keeps): after
                                                    * a stream the places' buffers did not fit ("places: no memory");
                                                    * the next stream takes a decoder again */
        memset(&pb, 0, sizeof pb);
        if (!(pc = malloc(PC_MAX * sizeof *pc))) {
            pl_fail("places: no memory");
            break;
        }
        plat_log("places: counting");
        pb.state = PB_COUNT;
        if (rgp_start(pl_place))
            pl_fail("places: busy");
        break;
    case PB_COUNT:
    case PB_BATCH:
        if (rgp_state == RGP_BUSY)
            break;
        if (rgp_state != RGP_DONE) {
            if (++pb.tries <= 3u) {                /* the download dropped: this pass again, from the start */
                if (pb.state == PB_COUNT)
                    pb.n_pc = 0;
                bbuf_used = 0;
                pb.overflow = 0;
                plat_log("places: again");
                if (rgp_start(pl_place))
                    pl_fail("places: busy");
                break;
            }
            pl_fail(rgp_why[0] ? rgp_why : "places: no answer");
            break;
        }
        pb.tries = 0;
        if (pb.state == PB_COUNT) {
            int rc;
            pb.list_kb = rgp_bytes / 1024u;
            rc = pl_plan();
            if (rc) {
                pl_fail(rc == -3 ? "places: too many" : "places: no memory");
                break;
            }
            pb.state = PB_ERASE;
            break;
        }
        if (pb.overflow) {
            pl_fail("places: the list changed");
            break;
        }
        if (pb.cur < pb.n_pc && pc[pc_order[pb.cur]].batch == pb.pass) {   /* the batch's countries: one a pass of
                                                   * the main loop. A whole batch at once (each country a scan of the
                                                   * batch, a sort and its flash writes) held WiFi's tasks off for
                                                   * seconds: its beacons went unread and the access point dropped us */
            if (pl_country(pc_order[pb.cur++]))
                pl_fail("places: flash write");
            break;
        }
        if (pb.pass < pb.passes) {
            pb.pass++;
            bbuf_used = 0;
            if (rgp_start(pl_place))
                pl_fail("places: busy");
            break;
        }
        pb.state = PB_WRITE;
        break;
    case PB_ERASE:                                 /* a sector a pass of the main loop (each stops all for ~40 ms) */
        if (pb.erase_at < pb.erase_end) {
            if (plat_data_erase(pb.erase_at)) {
                pl_fail("places: flash erase");
                break;
            }
            pb.erase_at += 0x1000u;
            break;
        }
        pb.pass = 1;
        bbuf_used = 0;
        pb.state = PB_BATCH;
        if (rgp_start(pl_place))
            pl_fail("places: busy");
        break;
    case PB_WRITE:
        if (pl_tables()) {
            pl_fail("places: flash write");
            break;
        }
        pl_free();
        pb.state = PB_IDLE;
        rg_open();
        {
            char b[40];
            snprintf(b, sizeof b, "places: %u in %u countries", (unsigned)(rg ? rg->n_place : 0), pb.n_pc);
            plat_log(b);
        }
        radio_ui_restore();
        ui_dirty = 1;
        break;
    }
}

/* for the screen: what the fetch is doing, or 0 when there is a table */
static const char *places_status(char *b, unsigned max)
{
    if (rg)
        return 0;
    if (wifi_st != WS_CONNECTED)
        w_cpy(b, plat_wifi_saved(0, 0) ? "waiting for WiFi" : "no WiFi: GLO to join a network", max);
    else if (pb.state == PB_FAILED)
        snprintf(b, max, "%s: again soon", pb.why);
    else if (pb.state == PB_COUNT || pb.state == PB_BATCH || pb.state == PB_ERASE) {
        /* the time left: this part's rest, then the parts to come, at the paced ~48 KB/s (radio_net.c) */
        uint32_t list = pb.list_kb ? pb.list_kb : 1900u, got = pb.state == PB_ERASE ? 0u : rgp_bytes / 1024u;
        uint32_t part = (list > got ? list - got : 0u) * 1024u / 48000u;   /* seconds */
        uint32_t later = pb.state == PB_COUNT ? 3u : pb.passes - (pb.state == PB_ERASE ? 0u : pb.pass);
        uint32_t all = part + later * (list * 1024u / 48000u + 2u);
        if (pb.state == PB_COUNT)
            snprintf(b, max, "counting: %u:%02u, about %u:%02u in all", part / 60u, part % 60u, all / 60u, all % 60u);
        else
            snprintf(b, max, "part %u of %u: %u:%02u, %u:%02u in all", pb.state == PB_ERASE ? 1u : pb.pass,
                     pb.passes, part / 60u, part % 60u, all / 60u, all % 60u);
    } else
        w_cpy(b, "starting", max);
    return b;
}

/* fetch them again (radio_ctl.py places): the old table's header erased, a new one built */
static void places_refresh(void)
{
    if (pb.state != PB_IDLE && pb.state != PB_FAILED)
        return;
    plat_data_erase(RGP_OFF);
    rg = 0;
    pb.state = PB_IDLE;
    pl_first = 1;
    ui_dirty = 1;
}
