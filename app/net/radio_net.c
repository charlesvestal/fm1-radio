/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* The radio's network side, built against the SDK's lwIP headers (sockets, DNS); the app sees only
 * radio_net_* and rgn_*.
 *  - The stream: a task that fetches an HTTP or HTTPS stream (Icecast / SHOUTcast) into a ring the app's
 *    decoder drains. Redirects (3xx Location) are followed a few times. A stream that is not MP3 (AAC, HLS,
 *    a page) is refused by its Content-Type.
 *  - Radio Garden: a task that finds a place by name (its search) and reads the place's stations (its
 *    channels page), as the JSON arrives, into a list the app browses. A station plays through Radio
 *    Garden's listen link, which redirects to the real stream.
 * HTTPS is BearSSL's client (third_party/bearssl), TLS 1.2 with ECDHE and AES-GCM. The server's
 * certificate is NOT checked (no roots, no clock): its key is used as sent. For a radio, the worst a man
 * in the middle can do is change the audio or the station list. One TLS connection at a time: the app
 * stops an https stream before it asks Radio Garden. */
#include <stdint.h>
#include <string.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "shim.h"
#include "bearssl.h"

extern int snprintf(char *buf, unsigned long sz, const char *f, ...);
extern void *malloc(unsigned long n);
extern void free(void *p);
extern void radio_entropy(uint8_t *p, unsigned n);   /* the app's: timing and converter noise */

enum { RN_IDLE, RN_CONNECTING, RN_STREAMING, RN_ERROR, RN_ENDED, RN_UNPLAYABLE };
volatile int radio_net_state;
volatile uint32_t radio_rd, radio_wr;            /* the ring: the task writes, the app reads (bytes, ever) */
static uint8_t *ring;
static uint32_t ring_size;
#define URL_MAX 512u                              /* tokenised redirects (zeno.fm) run to ~350 */
static char *url;                                 /* URL_MAX, from the heap once (RAM is full) */
static volatile uint8_t stop_req, running;
char radio_net_why[32];
char radio_net_name[48];                          /* icy-name, when the station sends one */

static void why_to(char *w, const char *s)
{
    uint32_t i;
    for (i = 0; i + 1u < 32u && s[i]; i++)
        w[i] = s[i];
    w[i] = 0;
    shim_log(s);
}

/* ---- the connection: a socket, and TLS over it for https */
typedef struct {
    const br_x509_class *vtable;                  /* no checks: the first certificate's key, as sent */
    br_x509_decoder_context dec;
    int depth;
} nov_t;
typedef struct {
    br_ssl_client_context cc;
    nov_t x509;
    br_sslio_context io;
} tls_t;
typedef struct {
    int sock;
    tls_t *tls;                                   /* &tls_state while this connection holds it */
    unsigned char *buf;                           /* its 16.7 KB record buffer, from the heap */
    char *why;                                    /* where its errors are told (32 bytes) */
} conn_t;
/* the TLS state (4.5 KB) in the NOINIT region the smaller log left free: one connection at a time */
static tls_t tls_state __attribute__((section(".noinit")));
static volatile uint8_t tls_taken;

static void nov_start_chain(const br_x509_class **c, const char *name) { (void)name; ((nov_t *)c)->depth = 0; }
static void nov_start_cert(const br_x509_class **c, uint32_t len)
{
    nov_t *x = (nov_t *)c;
    (void)len;
    if (x->depth == 0)
        br_x509_decoder_init(&x->dec, 0, 0);
}
static void nov_append(const br_x509_class **c, const unsigned char *b, size_t n)
{
    nov_t *x = (nov_t *)c;
    if (x->depth == 0)
        br_x509_decoder_push(&x->dec, b, n);
}
static void nov_end_cert(const br_x509_class **c) { ((nov_t *)c)->depth++; }
static unsigned nov_end_chain(const br_x509_class **c)
{
    nov_t *x = (nov_t *)c;
    return x->depth && br_x509_decoder_get_pkey(&x->dec) ? 0u : (unsigned)BR_ERR_X509_EMPTY_CHAIN;
}
static const br_x509_pkey *nov_get_pkey(const br_x509_class *const *c, unsigned *usages)
{
    if (usages)
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return br_x509_decoder_get_pkey(&((nov_t *)c)->dec);
}
static const br_x509_class NOV = {sizeof(nov_t), nov_start_chain, nov_start_cert, nov_append, nov_end_cert,
                                  nov_end_chain, nov_get_pkey};

static int sock_rd(void *ctx, unsigned char *b, size_t n)
{
    int r = recv(*(int *)ctx, b, n, 0);
    return r > 0 ? r : -1;
}
static int sock_wr(void *ctx, const unsigned char *b, size_t n)
{
    int r = send(*(int *)ctx, b, n, 0);
    return r > 0 ? r : -1;
}

/* what streaming servers offer: ECDHE with AES-GCM (P-256 for the exchange and ECDSA keys, or RSA keys) */
static const uint16_t SUITES[] = {
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384};

static int tls_open(conn_t *c, const char *host)
{
    br_ssl_engine_context *e;
    uint8_t seed[32];
    if (tls_taken)
        return -2;                                /* the other task holds it */
    c->buf = malloc(BR_SSL_BUFSIZE_MONO);
    if (!c->buf)
        return -1;
    tls_taken = 1;
    c->tls = &tls_state;
    e = &c->tls->cc.eng;
    br_ssl_client_zero(&c->tls->cc);
    br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(e, SUITES, sizeof SUITES / sizeof SUITES[0]);
    br_ssl_engine_set_rsavrfy(e, &br_rsa_i31_pkcs1_vrfy);
    br_ssl_engine_set_ec(e, &br_ec_p256_m31);
    br_ssl_engine_set_ecdsa(e, &br_ecdsa_i31_vrfy_asn1);
    br_ssl_engine_set_hash(e, br_sha1_ID, &br_sha1_vtable);
    br_ssl_engine_set_hash(e, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(e, br_sha384_ID, &br_sha384_vtable);
    br_ssl_engine_set_prf_sha256(e, &br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(e, &br_tls12_sha384_prf);
    br_ssl_engine_set_gcm(e, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_aes_ctr(e, &br_aes_small_ctr_vtable);
    br_ssl_engine_set_ghash(e, &br_ghash_ctmul32);
    c->tls->x509.vtable = &NOV;
    br_ssl_engine_set_x509(e, &c->tls->x509.vtable);
    br_ssl_engine_set_buffer(e, c->buf, BR_SSL_BUFSIZE_MONO, 0);
    radio_entropy(seed, sizeof seed);
    br_ssl_engine_inject_entropy(e, seed, sizeof seed);
    if (!br_ssl_client_reset(&c->tls->cc, host, 0)) {
        free(c->buf);
        c->buf = 0;
        c->tls = 0;
        tls_taken = 0;
        return -1;
    }
    br_sslio_init(&c->tls->io, e, sock_rd, &c->sock, sock_wr, &c->sock);
    return 0;
}

static void conn_close(conn_t *c)
{
    if (c->tls) {                                 /* no close_notify: the socket goes too */
        free(c->buf);
        c->buf = 0;
        c->tls = 0;
        tls_taken = 0;
    }
    if (c->sock >= 0)
        closesocket(c->sock);
    c->sock = -1;
}
static int conn_read(conn_t *c, void *b, unsigned n)   /* > 0 bytes, 0 the end, < 0 an error or a timeout */
{
    int r, err;
    if (!c->tls)
        return recv(c->sock, b, n, 0);
    r = br_sslio_read(&c->tls->io, b, n);
    if (r >= 0)
        return r;
    err = br_ssl_engine_last_error(&c->tls->cc.eng);
    if (err && err != BR_ERR_IO)
        snprintf(c->why, 32, "tls error %d", err);
    return err == 0 ? 0 : -1;
}
static int conn_write(conn_t *c, const void *b, unsigned n)
{
    if (!c->tls)
        return send(c->sock, b, n, 0) == (int)n ? 0 : -1;
    if (br_sslio_write_all(&c->tls->io, b, n) < 0 || br_sslio_flush(&c->tls->io) < 0) {
        snprintf(c->why, 32, "tls error %d", br_ssl_engine_last_error(&c->tls->cc.eng));
        return -1;
    }
    return 0;
}

/* "http[s]://host[:port]/path" */
static int split(const char *u, char *host, unsigned hmax, int *port, const char **path, int *secure)
{
    unsigned n = 0;
    *secure = !strncmp(u, "https://", 8);
    if (strncmp(u, "http://", 7) && !*secure)
        return -1;
    u += *secure ? 8 : 7;
    while (*u && *u != '/' && *u != ':' && n + 1u < hmax)
        host[n++] = *u++;
    host[n] = 0;
    *port = *secure ? 443 : 80;
    if (*u == ':') {
        *port = 0;
        for (u++; *u >= '0' && *u <= '9'; u++)
            *port = *port * 10 + (*u - '0');
    }
    *path = *u ? u : "/";
    return n ? 0 : -1;
}

static int ieq_prefix(const char *a, const char *p)    /* a starts with p, ignoring case */
{
    for (; *p; a++, p++) {
        char x = *a, y = *p;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return 0;
    }
    return 1;
}
static int has_word(const char *s, const char *w)      /* w somewhere in s, ignoring case */
{
    for (; *s; s++)
        if (ieq_prefix(s, w))
            return 1;
    return 0;
}

/* one request on c for u (u is replaced by a redirect's target, umax its room); json: an API call (no ICY
 * name, any Content-Type). 0 = the body follows, 1 = redirected, -1 = failed, -2 = not an MP3 stream */
static int http_open(conn_t *c, char *u, unsigned umax, int json)
{
    char host[64], req[640], line[URL_MAX];
    const char *path;
    int port, s, n = 0, status = 0, secure, rc;
    struct hostent *he;
    struct sockaddr_in a;
    char type[40];
    type[0] = 0;
    if (split(u, host, sizeof host, &port, &path, &secure)) {
        why_to(c->why, "not an http(s):// url");
        return -1;
    }
    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        why_to(c->why, "dns failed");
        return -1;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        why_to(c->why, "no socket");
        return -1;
    }
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    memcpy(&a.sin_addr, he->h_addr_list[0], 4);
    {
        const uint8_t *ip = (const uint8_t *)he->h_addr_list[0];
        snprintf(req, 48, "%s = %u.%u.%u.%u:%d", host, ip[0], ip[1], ip[2], ip[3], port);
        shim_log(req);
    }
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
        closesocket(s);
        snprintf(c->why, 32, "connect failed (%d)", err);
        shim_log(c->why);
        return -1;
    }
    {   /* a silent server ends the recv after 5 s (lwIP's NONSTANDARD option: an int, ms), so it can reconnect */
        int ms = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &ms, sizeof ms);
    }
    c->sock = s;
    if (secure && (rc = tls_open(c, host)) != 0) {
        conn_close(c);
        why_to(c->why, rc == -2 ? "tls busy" : "tls: no memory");
        return -1;
    }
    n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: FM-1 Net/0.1\r\n%s"
                 "Connection: close\r\n\r\n", path, host,
                 json ? "Accept: application/json\r\n" : "Icy-MetaData: 0\r\nAccept: */*\r\n");
    if (conn_write(c, req, (unsigned)n)) {        /* for https, the handshake happens here */
        conn_close(c);
        if (!c->why[0] || !secure)
            why_to(c->why, "send failed");
        shim_log(c->why);
        return -1;
    }
    if (secure)
        shim_log("tls up");
    /* the headers, a line at a time */
    if (!json)
        radio_net_name[0] = 0;
    for (;;) {
        unsigned k = 0;
        char ch = 0;
        while (k + 1u < sizeof line) {
            if (conn_read(c, &ch, 1) != 1) {
                conn_close(c);
                if (!c->why[0])
                    why_to(c->why, "closed in headers");
                shim_log(c->why);
                return -1;
            }
            if (ch == '\n')
                break;
            if (ch != '\r')
                line[k++] = ch;
        }
        if (ch != '\n')                           /* too long: the rest of the line goes */
            while (conn_read(c, &ch, 1) == 1 && ch != '\n')
                ;
        line[k] = 0;
        if (!status) {                            /* "HTTP/1.x 200 OK" or "ICY 200 OK" */
            const char *p = line;
            while (*p && *p != ' ')
                p++;
            while (*p == ' ')
                p++;
            while (*p >= '0' && *p <= '9')
                status = status * 10 + (*p++ - '0');
            snprintf(req, 40, "http %d", status);
            shim_log(req);
            continue;
        }
        if (!k)
            break;                                /* the blank line: the body follows */
        if (status >= 300 && status < 400 && ieq_prefix(line, "location:")) {
            const char *v = line + 9;
            while (*v == ' ')
                v++;
            strncpy(u, v, umax - 1u);
            u[umax - 1u] = 0;
        } else if (!json && ieq_prefix(line, "icy-name:")) {
            const char *v = line + 9;
            while (*v == ' ')
                v++;
            strncpy(radio_net_name, v, sizeof radio_net_name - 1);
            radio_net_name[sizeof radio_net_name - 1] = 0;
        } else if (ieq_prefix(line, "content-type:")) {
            const char *v = line + 13;
            while (*v == ' ')
                v++;
            strncpy(type, v, sizeof type - 1);
            type[sizeof type - 1] = 0;
        }
    }
    if (status >= 300 && status < 400) {
        conn_close(c);
        shim_log("redirect");
        return 1;
    }
    if (status != 200) {
        conn_close(c);
        if (!json && (status == 404 || status == 410)) {   /* gone: Radio Garden's link to an offline station */
            why_to(c->why, "station offline");
            return -2;
        }
        snprintf(c->why, 32, "http status %d", status);
        shim_log(c->why);
        return -1;
    }
    /* a stream the decoder cannot play: AAC (aac, aacp, mp4), HLS (mpegurl), Ogg, a web page */
    if (!json && type[0] && (has_word(type, "aac") || has_word(type, "mp4") || has_word(type, "mpegurl") ||
                             has_word(type, "ogg") || has_word(type, "html"))) {
        conn_close(c);
        snprintf(c->why, 32, "can't play %s", has_word(type, "aac") || has_word(type, "mp4") ? "AAC" :
                 has_word(type, "mpegurl") ? "HLS" : has_word(type, "ogg") ? "Ogg" : "a page");
        shim_log(c->why);
        return -2;
    }
    return 0;
}

/* ---- the stream */
static conn_t sc = {-1, 0, 0, radio_net_why};

static void radio_task(void *arg)
{
    int tries, rc = -1;
    (void)arg;
    running = 1;
    radio_net_state = RN_CONNECTING;
    for (tries = 0; tries < 4 && !stop_req; tries++) {
        rc = http_open(&sc, url, URL_MAX, 0);
        if (rc != 1)
            break;
    }
    if (rc != 0) {
        radio_net_state = rc == -2 ? RN_UNPLAYABLE : RN_ERROR;
        running = 0;
        return;
    }
    radio_net_state = RN_STREAMING;
    while (!stop_req) {
        uint32_t used = radio_wr - radio_rd, room = ring_size - used, at = radio_wr % ring_size, n;
        int got;
        if (room < 1460u) {                       /* full: let the decoder catch up */
            os_time_dly(1);
            continue;
        }
        n = ring_size - at;                       /* the contiguous part */
        if (n > room)
            n = room;
        if (n > 2920u)
            n = 2920u;
        got = conn_read(&sc, ring + at, n);
        if (got <= 0) {
            if (got == 0 || !sc.tls)
                why_to(radio_net_why, got == 0 ? "stream ended" : "stream stalled");
            else if (!radio_net_why[0])
                why_to(radio_net_why, "stream stalled");
            radio_net_state = got == 0 ? RN_ENDED : RN_ERROR;
            break;
        }
        radio_wr += (uint32_t)got;
    }
    conn_close(&sc);
    if (stop_req)
        radio_net_state = RN_IDLE;
    running = 0;
}

/* the app's: start fetching url into buf (a power of two is not needed); 0 = started */
int radio_net_start(const char *u, uint8_t *buf, uint32_t size)
{
    if (running)
        return -1;
    if (!url && !(url = malloc(URL_MAX)))
        return -1;
    strncpy(url, u, URL_MAX - 1);
    url[URL_MAX - 1] = 0;
    ring = buf;
    ring_size = size;
    radio_rd = radio_wr = 0;
    radio_net_why[0] = 0;
    stop_req = 0;
    radio_net_state = RN_CONNECTING;              /* now, not when the task first runs: the last stream's state (an */
    running = 1;                                  /* unplayable one) was read meanwhile, with no reason: ": turn K4" */
    if (task_create(radio_task, 0, "radio")) {
        running = 0;
        radio_net_state = RN_ERROR;
        why_to(radio_net_why, "no task");
        return -1;
    }
    return 0;
}
/* ask the task to stop; it ends after its current recv */
void radio_net_stop(void) { stop_req = 1; }
int radio_net_running(void) { return running; }
int radio_net_tls(void) { return sc.tls != 0; }   /* the stream holds the TLS state */

/* ---- Radio Garden: a place's stations */

/* U+00C0..U+017F as the ASCII letter the places table keeps (rg_places.py: NFKD, marks dropped), or 0 */
static const char FOLD[] =
    "AAAAAA\0CEEEEIIII\0NOOOOO\0\0UUUUY\0\0"
    "aaaaaa\0ceeeeiiii\0nooooo\0\0uuuuy\0y"
    "AaAaAaCcCcCcCcDd\0\0EeEeEeEeEeGgGg"
    "GgGgHh\0\0IiIiIiIiI\0IiJjKk\0LlLlLlL"
    "l\0\0NnNnNnn\0\0OoOoOo\0\0RrRrRrSsSsSs"
    "SsTtTt\0\0UuUuUuUuUuUuWwYyYZzZzZzs";
static char fold_cp(uint32_t cp)                  /* a code point as the table's ASCII, or 0 to drop it */
{
    if (cp >= 32u && cp < 127u)
        return (char)cp;
    if (cp >= 0xC0u && cp < 0x180u)
        return FOLD[cp - 0xC0u];
    return 0;
}

#define RGN_MAX 100u                              /* stations kept from a place (Radio Garden's order) */
typedef struct { char id[12], title[32]; } rgn_station_t;
enum { RGN_IDLE, RGN_BUSY, RGN_DONE, RGN_ERROR };
volatile int rgn_state;
rgn_station_t *rgn_list;                          /* RGN_MAX, from the heap once */
volatile uint32_t rgn_n;
char rgn_why[32];
static char rgn_place[32], rgn_country[32], rgn_url[200];

/* a streaming JSON reader, just enough for Radio Garden's answers: every object's "url", "title" and
 * "subtitle" strings, handed over when the object closes. Text is folded to ASCII as it is read. */
#define JS_DEPTH 16
typedef struct {
    uint8_t depth, in_str, esc, uhex, is_key, want_key[JS_DEPTH], is_obj[JS_DEPTH];
    uint32_t ucp;
    uint32_t cp, utf_left;                        /* a UTF-8 sequence being read */
    char key[12], val[96];
    uint8_t klen, vlen;
    char url[JS_DEPTH][40], title[JS_DEPTH][32], sub[JS_DEPTH][32];
    int (*obj)(void *, const char *url, const char *title, const char *sub);   /* nonzero: stop */
    void *ctx;
} js_t;

static void js_char(js_t *j, uint32_t cp)        /* a character of the string being read */
{
    char ch = fold_cp(cp);
    if (!ch)
        return;
    if (j->is_key) {
        if (j->klen + 1u < sizeof j->key)
            j->key[j->klen++] = ch;
    } else if (j->vlen + 1u < sizeof j->val) {
        j->val[j->vlen++] = ch;
    }
}
static void js_put(char *d, unsigned max, const char *s, unsigned n)
{
    if (n >= max)
        n = max - 1u;
    memcpy(d, s, n);
    d[n] = 0;
}
static int js_feed(js_t *j, const uint8_t *p, unsigned n)   /* nonzero: the caller has what it wants */
{
    unsigned i;
    for (i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (j->in_str) {
            if (j->uhex) {                        /* \uXXXX */
                uint32_t v = b >= '0' && b <= '9' ? b - '0' : (b | 32u) >= 'a' && (b | 32u) <= 'f' ? (b | 32u) - 'a' + 10u : 0u;
                j->ucp = j->ucp << 4 | v;
                if (!--j->uhex)
                    js_char(j, j->ucp);
            } else if (j->esc) {
                j->esc = 0;
                if (b == 'u') {
                    j->uhex = 4;
                    j->ucp = 0;
                } else {
                    js_char(j, b == 'n' || b == 't' || b == 'r' ? ' ' : b);
                }
            } else if (b == '\\') {
                j->esc = 1;
            } else if (b == '"') {
                j->in_str = 0;
                if (j->is_key) {
                    j->key[j->klen] = 0;
                } else if (j->depth && j->depth <= JS_DEPTH) {   /* a value: kept if it is one we want */
                    uint8_t d = (uint8_t)(j->depth - 1u);
                    j->val[j->vlen] = 0;
                    if (!strcmp(j->key, "url"))
                        js_put(j->url[d], sizeof j->url[d], j->val, j->vlen);
                    else if (!strcmp(j->key, "title"))
                        js_put(j->title[d], sizeof j->title[d], j->val, j->vlen);
                    else if (!strcmp(j->key, "subtitle"))
                        js_put(j->sub[d], sizeof j->sub[d], j->val, j->vlen);
                }
            } else if (j->utf_left) {             /* UTF-8 continuation */
                j->cp = j->cp << 6 | (b & 0x3Fu);
                if (!--j->utf_left)
                    js_char(j, j->cp);
            } else if (b >= 0xC0u) {
                j->utf_left = b >= 0xF0u ? 3u : b >= 0xE0u ? 2u : 1u;
                j->cp = b & (b >= 0xF0u ? 0x07u : b >= 0xE0u ? 0x0Fu : 0x1Fu);
            } else {
                js_char(j, b);
            }
            continue;
        }
        if (b == '"') {
            j->in_str = 1;
            j->esc = j->uhex = 0;
            j->utf_left = 0;
            j->is_key = j->depth && j->depth <= JS_DEPTH && j->want_key[j->depth - 1u];
            if (j->is_key)
                j->klen = 0;
            else
                j->vlen = 0;
        } else if (b == '{' || b == '[') {
            if (j->depth < JS_DEPTH) {
                uint8_t d = j->depth;
                j->is_obj[d] = j->want_key[d] = b == '{';
                j->url[d][0] = j->title[d][0] = j->sub[d][0] = 0;
            }
            j->depth++;
            j->key[0] = 0;
        } else if (b == '}' || b == ']') {
            if (j->depth && j->depth <= JS_DEPTH && b == '}') {
                uint8_t d = (uint8_t)(j->depth - 1u);
                if (j->url[d][0] && j->obj(j->ctx, j->url[d], j->title[d], j->sub[d]))
                    return 1;
            }
            if (j->depth)
                j->depth--;
        } else if (b == ':') {
            if (j->depth && j->depth <= JS_DEPTH)
                j->want_key[j->depth - 1u] = 0;
        } else if (b == ',') {
            if (j->depth && j->depth <= JS_DEPTH)
                j->want_key[j->depth - 1u] = j->is_obj[j->depth - 1u];   /* in an object, a key next */
        }
    }
    return 0;
}

static int ieq(const char *a, const char *b)       /* equal, ignoring case */
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return 0;
    }
    return *a == *b;
}
static const char *last_seg(const char *u)        /* "/visit/amsterdam/WYa4vKGn" -> "WYa4vKGn" */
{
    const char *s = u;
    for (; *u; u++)
        if (*u == '/' && u[1])
            s = u + 1;
    return s;
}

typedef struct { char exact[12], near[12]; } rgn_find_t;
static int on_place(void *ctx, const char *u, const char *title, const char *sub)
{
    rgn_find_t *f = ctx;
    if (strncmp(u, "/visit/", 7))
        return 0;
    if (ieq(title, rgn_place) && (!sub[0] || ieq(sub, rgn_country))) {
        js_put(f->exact, sizeof f->exact, last_seg(u), strlen(last_seg(u)));
        return 1;
    }
    if (!f->near[0] && ieq(title, rgn_place))        /* the name, in another country: if nothing better */
        js_put(f->near, sizeof f->near, last_seg(u), strlen(last_seg(u)));
    return 0;
}
static int on_station(void *ctx, const char *u, const char *title, const char *sub)
{
    const char *id;
    uint32_t i;
    (void)ctx;
    (void)sub;
    if (strncmp(u, "/listen/", 8) || rgn_n >= RGN_MAX)
        return rgn_n >= RGN_MAX;
    id = last_seg(u);
    for (i = 0; i < rgn_n; i++)                   /* once each */
        if (!strcmp(rgn_list[i].id, id))
            return 0;
    js_put(rgn_list[rgn_n].id, sizeof rgn_list[rgn_n].id, id, strlen(id));
    js_put(rgn_list[rgn_n].title, sizeof rgn_list[rgn_n].title, title[0] ? title : id, strlen(title[0] ? title : id));
    rgn_n++;
    return 0;
}

static int url_add(char *d, unsigned max, const char *s)   /* s appended, as a query parameter */
{
    unsigned n = (unsigned)strlen(d);
    for (; *s && n + 4u < max; s++) {
        uint8_t ch = (uint8_t)*s;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.')
            d[n++] = (char)ch;
        else if (ch == ' ')
            d[n++] = '+';
        else {
            d[n++] = '%';
            d[n++] = "0123456789ABCDEF"[ch >> 4];
            d[n++] = "0123456789ABCDEF"[ch & 15u];
        }
    }
    d[n] = 0;
    return 0;
}

/* GET rgn_url and run its JSON through the reader */
static int rgn_get(js_t *j)
{
    conn_t c = {-1, 0, 0, rgn_why};
    uint8_t b[256];
    int rc, tries, got;
    for (tries = 0; tries < 3; tries++) {
        rc = http_open(&c, rgn_url, sizeof rgn_url, 1);
        if (rc != 1)
            break;
    }
    if (rc != 0)
        return -1;
    while ((got = conn_read(&c, b, sizeof b)) > 0)
        if (js_feed(j, b, (unsigned)got))
            break;
    conn_close(&c);
    return got < 0 ? -1 : 0;
}

static void rgn_task(void *arg)
{
    js_t *j = malloc(sizeof *j);
    rgn_find_t f;
    (void)arg;
    if (!j) {
        why_to(rgn_why, "no memory");
        rgn_state = RGN_ERROR;
        return;
    }
    /* the place, by name: Radio Garden's search */
    memset(j, 0, sizeof *j);
    memset(&f, 0, sizeof f);
    j->obj = on_place;
    j->ctx = &f;
    strcpy(rgn_url, "https://radio.garden/api/search?q=");
    url_add(rgn_url, sizeof rgn_url, rgn_place);
    strcat(rgn_url, "+");
    url_add(rgn_url, sizeof rgn_url, rgn_country);
    if (rgn_get(j) || (!f.exact[0] && !f.near[0])) {
        if (!rgn_why[0])
            why_to(rgn_why, "place not found");
        free(j);
        rgn_state = RGN_ERROR;
        return;
    }
    /* its stations */
    memset(j, 0, sizeof *j);
    j->obj = on_station;
    snprintf(rgn_url, sizeof rgn_url, "https://radio.garden/api/ara/content/page/%s/channels",
             f.exact[0] ? f.exact : f.near);
    if (rgn_get(j) && !rgn_n) {
        free(j);
        rgn_state = RGN_ERROR;
        return;
    }
    free(j);
    rgn_state = RGN_DONE;
}

/* the app's: find place (in country) and read its stations into rgn_list; 0 = started */
int rgn_start(const char *place, const char *country)
{
    if (rgn_state == RGN_BUSY)
        return -1;
    if (!rgn_list && !(rgn_list = malloc(RGN_MAX * sizeof *rgn_list)))
        return -1;
    js_put(rgn_place, sizeof rgn_place, place, strlen(place));
    js_put(rgn_country, sizeof rgn_country, country, strlen(country));
    rgn_n = 0;
    rgn_why[0] = 0;
    rgn_state = RGN_BUSY;
    if (task_create(rgn_task, 0, "rgfetch")) {
        rgn_state = RGN_ERROR;
        why_to(rgn_why, "no task");
        return -1;
    }
    return 0;
}
