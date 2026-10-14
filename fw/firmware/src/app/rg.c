/* Radio Garden's stations, as tools/rg_sync.py writes them, kept in the data flash: the table from RG_TABLE,
 * a commit record at RG_HEAD written last (a torn upload leaves no table). Read in place through the plain
 * XIP window (fl_plain_window_init). Table format (little-endian), see rg_sync.py:
 *   header 64 B: magic "RGT1", version, n_continents/countries/cities/stations, the offsets, total_len,
 *   CRC-32 of [64, total_len); then continents / countries / cities {name, first child, n children} and
 *   stations {name, url}, 8 bytes each; then NUL-terminated ASCII strings.
 * Upload over the wire (F0 7D 57 ...): 0x50 len:5 begin, 0x51 off:3 data (8-to-7 packed) -> 0x10 "rg next",
 * 0x52 crc:5 end, 0x53 info. Part of the unity build (main_wifi.c). */
#define RG_HEAD 0x97000u
#define RG_TABLE 0x98000u
#define RG_MAX 0x44000u                           /* to 0xDC000: OBJ_PANEL's sectors follow */
#define RG_MAGIC 0x31544752u                      /* "RGT1" */
#define RG_COMMIT 0x4B4F4752u                     /* "RGOK" */

typedef struct {
    uint32_t magic;
    uint16_t version, n_cont, n_country, n_city, n_station, rsv;
    uint32_t off_cont, off_country, off_city, off_station, off_str, total, crc, created, pad[4];
} rg_hdr_t;
typedef struct { uint32_t name; uint16_t first, n; } rg_node_t;
typedef struct { uint32_t name, url; } rg_station_t;
typedef struct { uint32_t magic, len, crc, rsv; } rg_commit_t;

static const rg_hdr_t *rg;                        /* the table, or 0 */

static const void *rg_xip(uint32_t off) { return (const void *)fm1_xip_ptr(off); }

/* the CRC of flash [off, off+n), read straight from the chip: a long stream through the XIP cache (with WiFi
 * busy) has fetched bad code (a jump to 0x470) */
static uint32_t rg_crc_flash(uint32_t off, uint32_t n)   /* zlib CRC-32 (Mocky's nibble table) */
{
    static const uint32_t T[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
    uint8_t b[256];
    uint32_t c = 0xFFFFFFFFu;
    while (n) {
        uint32_t k = n > sizeof b ? (uint32_t)sizeof b : n, i;
        if (fl_read(off, b, k))
            return 0;
        for (i = 0; i < k; i++) {
            c ^= b[i];
            c = (c >> 4) ^ T[c & 15u];
            c = (c >> 4) ^ T[c & 15u];
        }
        off += k;
        n -= k;
        SHIM_WDT_FEED();
    }
    return ~c;
}

static void rg_open(void)
{
    const rg_commit_t *c = rg_xip(RG_HEAD);
    const rg_hdr_t *h = rg_xip(RG_TABLE);
    rg = 0;
    if (c->magic != RG_COMMIT || h->magic != RG_MAGIC || h->total != c->len || c->len > RG_MAX ||
        c->len < sizeof *h || h->crc != c->crc)
        return;
    if (rg_crc_flash(RG_TABLE + (uint32_t)sizeof *h, h->total - (uint32_t)sizeof *h) != h->crc)
        return;
    rg = h;
}

static const char *rg_str(uint32_t off) { return (const char *)rg + off; }
static const rg_node_t *rg_cont(uint32_t i) { return (const rg_node_t *)((const uint8_t *)rg + rg->off_cont) + i; }
static const rg_node_t *rg_country(uint32_t i) { return (const rg_node_t *)((const uint8_t *)rg + rg->off_country) + i; }
static const rg_node_t *rg_city(uint32_t i) { return (const rg_node_t *)((const uint8_t *)rg + rg->off_city) + i; }
static const rg_station_t *rg_station(uint32_t i)
{
    return (const rg_station_t *)((const uint8_t *)rg + rg->off_station) + i;
}

/* ---- the upload (the main loop's: flash writes stop the code fetches, so not from inside a task's wait) */
static struct {
    uint32_t len, got, erased, on;                /* erased: table bytes erased so far (whole sectors) */
    uint32_t chunk_off, chunk_len;                /* a decoded chunk waiting for the main loop */
    uint8_t chunk[600];
} rgu OM_POOL;

static uint32_t rg_get7(const uint8_t *p, uint32_t k)
{
    uint32_t v = 0, i;
    for (i = 0; i < k; i++)
        v |= (uint32_t)(p[i] & 0x7Fu) << (7u * i);
    return v;
}

/* wire_service: a 0x51 frame's data (after off:3), unpacked into rgu.chunk; 0 = taken */
static int rg_take_chunk(const uint8_t *p, uint32_t n)
{
    uint32_t off, o = 0, i;
    if (!rgu.on || n < 3u || rgu.chunk_len)
        return -1;
    off = rg_get7(p, 3);
    p += 3;
    n -= 3u;
    while (n) {                                   /* 8 wire bytes carry 7: a high-bit byte, then 7 */
        uint32_t m = *p++, k = n - 1u < 7u ? n - 1u : 7u;
        for (i = 0; i < k && o < sizeof rgu.chunk; i++)
            rgu.chunk[o++] = (uint8_t)(p[i] | (((m >> i) & 1u) << 7));
        p += k;
        n -= k + 1u;
    }
    rgu.chunk_off = off;
    rgu.chunk_len = o;
    return 0;
}

static void rg_reply(const char *what, uint32_t v)
{
    char b[32];
    snprintf(b, sizeof b, "rg %s %u", what, v);
    wire_line(b);
}

static void rg_begin(uint32_t len)
{
    uint32_t took;
    if (!len || len > RG_MAX || !flash_ok) {
        rg_reply("error", 1);
        return;
    }
    if (radio_st != RS_OFF)
        radio_stop();
    rg = 0;
    if (fl_erase4k(RG_HEAD, &took)) {             /* no commit record: no table until the end */
        rg_reply("error", 2);
        return;
    }
    rgu.len = len;
    rgu.got = rgu.erased = rgu.chunk_len = 0;
    rgu.on = 1;
    rg_reply("next", 0);
}

/* the main loop: write a chunk that arrived (in order; a resend of an earlier one is answered, not written) */
static void rg_service(void)
{
    uint32_t off = rgu.chunk_off, n = rgu.chunk_len, took;
    if (!n)
        return;
    rgu.chunk_len = 0;
    if (off != rgu.got || off + n > rgu.len) {
        rg_reply("next", rgu.got);
        return;
    }
    while (rgu.erased < off + n) {
        if (fl_erase4k(RG_TABLE + rgu.erased, &took)) {
            rg_reply("error", 3);
            rgu.on = 0;
            return;
        }
        rgu.erased += 0x1000u;
    }
    if (fl_write(RG_TABLE + off, rgu.chunk, n)) {
        rg_reply("error", 4);
        rgu.on = 0;
        return;
    }
    rgu.got += n;
    rg_reply("next", rgu.got);
}

static void rg_end(uint32_t crc)
{
    rg_commit_t c;
    const rg_hdr_t *h = rg_xip(RG_TABLE);
    uint32_t i;
    if (!rgu.on || rgu.got != rgu.len) {
        rg_reply("error", 5);
        return;
    }
    rgu.on = 0;
    fl_inval(RG_TABLE, rgu.len);
    if (rg_crc_flash(RG_TABLE, rgu.len) != crc || h->magic != RG_MAGIC || h->total != rgu.len) {
        rg_reply("error", 6);
        return;
    }
    c.magic = RG_COMMIT;
    c.len = rgu.len;
    c.crc = h->crc;
    c.rsv = 0;
    for (i = 0; i < sizeof c; i++)
        rgu.chunk[i] = ((const uint8_t *)&c)[i];  /* the driver wants a RAM source */
    if (fl_write(RG_HEAD, rgu.chunk, sizeof c)) {
        rg_reply("error", 7);
        return;
    }
    fl_inval(RG_HEAD, 0x1000u);
    rg_open();
    rg_reply(rg ? "ok" : "error", rg ? rg->n_station : 8u);
}
