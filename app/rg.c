/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Radio Garden's places, as the FM-1 fetches them itself (places.c) into the flash's data area, read in place
 * (fm1-base plat_data). The place names, a country at a time, are unpacked as they are needed (inflate.c).
 * Layout (little-endian), as tools/rg_places.py writes it too (for the host simulator):
 *   header 64 B: magic "RGP1", version, continents, countries, places, the offsets, total, CRC-32 of
 *   [64, total), created; continents {name, first country, countries} 8 B; countries {name, block offset,
 *   block length, places, continent} 16 B; NUL-terminated ASCII strings; the blocks: a country's names,
 *   sorted, front coded (shared length, the rest, NUL), raw deflate (rg_places.py: in a 1 KB window; places.c:
 *   stored, as the FM-1 has no compressor).
 * Part of the app's unity build (app.c). */
#define RG_MAGIC 0x31504752u                      /* "RGP1" */
#define RG_OFF PLAT_DATA_LO                       /* where it is: places.c RGP_OFF */
#define RG_MAX (0xD6000u - PLAT_DATA_LO)          /* .. OBJ_FAV's sectors (app.h) */
static uint32_t pl_crc(const uint8_t *p, uint32_t n);

typedef struct {
    uint32_t magic;
    uint16_t version, n_cont, n_country, n_place;
    uint32_t off_cont, off_country, off_str, off_blocks, total, crc, created, pad[6];
} rg_hdr_t;
typedef char rg_hdr_is_64[sizeof(rg_hdr_t) == 64u ? 1 : -1];
typedef struct { uint32_t name; uint16_t first, n; } rg_cont_t;
typedef struct { uint32_t name, block, len; uint16_t n; uint8_t cont, rsv; } rg_country_t;

static const rg_hdr_t *rg;                        /* the table, or 0 */

static void rg_open(void)                         /* the table, if one is whole there (its CRC checked) */
{
    const rg_hdr_t *h = (const rg_hdr_t *)plat_data(RG_OFF);
    rg = h && h->magic == RG_MAGIC && h->total > 64u && h->total <= RG_MAX &&
         pl_crc((const uint8_t *)h + 64, h->total - 64u) == h->crc ? h : 0;
}

static const char *rg_str(uint32_t off) { return (const char *)rg + rg->off_str + off; }
static const rg_cont_t *rg_cont(uint32_t i) { return (const rg_cont_t *)((const uint8_t *)rg + rg->off_cont) + i; }
static const rg_country_t *rg_country(uint32_t i)
{
    return (const rg_country_t *)((const uint8_t *)rg + rg->off_country) + i;
}

/* a country's place names [first, first + n): name(i, ctx, k) for each, in order; front decoding as they are
 * unpacked, stopping after the last one wanted */
typedef struct {
    uint32_t idx, first, last;                    /* the place being decoded; the range wanted */
    char cur[32];
    uint8_t k, have_k;
    void (*name)(uint32_t i, const char *s, void *ctx);
    void *ctx;
} rg_names_t;
static int rg_names_byte(void *p, uint8_t c)
{
    rg_names_t *r = p;
    if (!r->have_k) {                             /* how many characters it shares with the last name */
        r->k = c < sizeof r->cur ? c : (uint8_t)(sizeof r->cur - 1u);
        r->have_k = 1;
        return 0;
    }
    if (c) {
        if (r->k + 1u < sizeof r->cur)
            r->cur[r->k++] = (char)c;
        return 0;
    }
    r->cur[r->k] = 0;
    r->have_k = 0;
    if (r->idx >= r->first)
        r->name(r->idx, r->cur, r->ctx);
    return ++r->idx > r->last;
}
static int rg_names(uint32_t country, uint32_t first, uint32_t n, void (*name)(uint32_t, const char *, void *),
                    void *ctx)
{
    const rg_country_t *c;
    rg_names_t r;
    if (!rg || country >= rg->n_country || !n)
        return -1;
    c = rg_country(country);
    memset(&r, 0, sizeof r);
    r.first = first;
    r.last = first + n - 1u;
    r.name = name;
    r.ctx = ctx;
    return inflate_mem((const uint8_t *)rg + c->block, c->len, rg_names_byte, &r);
}
static void rg_name_one(uint32_t i, const char *s, void *ctx) { (void)i; w_cpy(ctx, s, 32); }
static int rg_place_name(uint32_t country, uint32_t i, char out[32])   /* one place's name */
{
    out[0] = 0;
    return rg_names(country, i, 1, rg_name_one, out) || !out[0] ? -1 : 0;
}
typedef struct { const char *want; uint32_t found; } rg_find_t;
static void rg_find_name(uint32_t i, const char *s, void *ctx)
{
    rg_find_t *f = ctx;
    if (f->found == 0xFFFFFFFFu && w_eq(s, f->want))
        f->found = i;
}
static int32_t rg_place_index(uint32_t country, const char *name)   /* a place by its name, or -1 */
{
    rg_find_t f = {name, 0xFFFFFFFFu};
    if (!rg || country >= rg->n_country)
        return -1;
    rg_names(country, 0, rg_country(country)->n, rg_find_name, &f);
    return f.found == 0xFFFFFFFFu ? -1 : (int32_t)f.found;
}
