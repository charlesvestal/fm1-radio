/* The radio's network side: one task that fetches an HTTP or HTTPS stream (Icecast / SHOUTcast) into a
 * ring the app's decoder drains. Built against the SDK's lwIP headers (sockets, DNS); the app sees only
 * radio_net_*. Redirects (3xx Location) are followed a few times.
 * HTTPS is BearSSL's client (third_party/bearssl), TLS 1.2 with ECDHE and AES-GCM. The server's certificate is NOT checked (no roots, no clock): its key is used as sent.
 * For a radio, the worst a man in the middle can do is change the audio. */
#include <stdint.h>
#include <string.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "shim.h"
#include "bearssl.h"

extern int snprintf(char *buf, unsigned long sz, const char *f, ...);

enum { RN_IDLE, RN_CONNECTING, RN_STREAMING, RN_ERROR, RN_ENDED };
volatile int radio_net_state;
volatile uint32_t radio_rd, radio_wr;            /* the ring: the task writes, the app reads (bytes, ever) */
static uint8_t *ring;
static uint32_t ring_size;
#define URL_MAX 512u                              /* tokenised redirects (zeno.fm) run to ~350 */
static char *url;                                 /* URL_MAX, from the heap once (RAM is full) */
extern void *malloc(unsigned long n);
extern void free(void *p);
extern void radio_entropy(uint8_t *p, unsigned n);   /* the app's: timing and converter noise */
static volatile uint8_t stop_req, running;
char radio_net_why[32];
char radio_net_name[48];                          /* icy-name, when the station sends one */

static void why(const char *s)
{
    uint32_t i;
    for (i = 0; i + 1u < sizeof radio_net_why && s[i]; i++)
        radio_net_why[i] = s[i];
    radio_net_why[i] = 0;
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
static int sock = -1;
/* the state (4.5 KB) in the NOINIT region the smaller log left free; the 16.7 KB record buffer (one record
 * at a time, both ways) from the heap while an https stream is open */
static tls_t tls_state __attribute__((section(".noinit")));
static tls_t *tls;
static unsigned char *tls_buf;

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

static int tls_open(int s, const char *host)
{
    br_ssl_engine_context *e;
    uint8_t seed[32];
    tls_buf = malloc(BR_SSL_BUFSIZE_MONO);
    if (!tls_buf)
        return -1;
    tls = &tls_state;
    e = &tls->cc.eng;
    br_ssl_client_zero(&tls->cc);
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
    tls->x509.vtable = &NOV;
    br_ssl_engine_set_x509(e, &tls->x509.vtable);
    br_ssl_engine_set_buffer(e, tls_buf, BR_SSL_BUFSIZE_MONO, 0);
    radio_entropy(seed, sizeof seed);
    br_ssl_engine_inject_entropy(e, seed, sizeof seed);
    if (!br_ssl_client_reset(&tls->cc, host, 0)) {
        free(tls_buf);
        tls_buf = 0;
        tls = 0;
        return -1;
    }
    sock = s;
    br_sslio_init(&tls->io, e, sock_rd, &sock, sock_wr, &sock);
    return 0;
}

static void conn_close(void)
{
    if (tls) {                                    /* no close_notify: the socket goes too */
        free(tls_buf);
        tls_buf = 0;
        tls = 0;
    }
    if (sock >= 0)
        closesocket(sock);
    sock = -1;
}
static int conn_read(void *b, unsigned n)         /* > 0 bytes, 0 the end, < 0 an error or a timeout */
{
    if (!tls)
        return recv(sock, b, n, 0);
    {
        int r = br_sslio_read(&tls->io, b, n);
        if (r < 0) {
            int err = br_ssl_engine_last_error(&tls->cc.eng);
            if (err && err != BR_ERR_IO)
                snprintf(radio_net_why, sizeof radio_net_why, "tls error %d", err);
            return err == 0 ? 0 : -1;
        }
        return r;
    }
}
static int conn_write(const void *b, unsigned n)
{
    if (!tls)
        return send(sock, b, n, 0) == (int)n ? 0 : -1;
    if (br_sslio_write_all(&tls->io, b, n) < 0 || br_sslio_flush(&tls->io) < 0) {
        snprintf(radio_net_why, sizeof radio_net_why, "tls error %d", br_ssl_engine_last_error(&tls->cc.eng));
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

/* one request; 0 = streaming (headers read), 1 = redirected (url replaced), < 0 = failed */
static int open_stream(void)
{
    char host[64], req[640], line[URL_MAX];
    const char *path;
    int port, s, n = 0, status = 0, secure;
    struct hostent *he;
    struct sockaddr_in a;
    if (split(url, host, sizeof host, &port, &path, &secure)) {
        why("not an http(s):// url");
        return -1;
    }
    he = gethostbyname(host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        why("dns failed");
        return -1;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        why("no socket");
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
        snprintf(radio_net_why, sizeof radio_net_why, "connect failed (%d)", err);
        shim_log(radio_net_why);
        {   /* the route lwIP lacks: the default interface, its flags, address and gateway */
            struct netif *nd = netif_default, *n;
            snprintf(req, 48, "default %p fl %x", (void *)nd, nd ? nd->flags : 0);
            shim_log(req);
            for (n = netif_list; n; n = n->next) {
                const uint8_t *ip = (const uint8_t *)&n->ip_addr, *gw = (const uint8_t *)&n->gw;
                snprintf(req, 64, "%c%c fl %x %u.%u.%u.%u gw %u.%u.%u.%u", n->name[0], n->name[1], n->flags,
                         ip[0], ip[1], ip[2], ip[3], gw[0], gw[1], gw[2], gw[3]);
                shim_log(req);
            }
        }
        return -1;
    }
    {   /* a silent stream ends the recv after 5 s (lwIP's NONSTANDARD option: an int, ms), so it can reconnect */
        int ms = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &ms, sizeof ms);
    }
    sock = s;
    if (secure && tls_open(s, host)) {
        conn_close();
        why("tls: no memory");
        return -1;
    }
    n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: FM-1 Net/0.1\r\nIcy-MetaData: 0\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (conn_write(req, (unsigned)n)) {           /* for https, the handshake happens here */
        conn_close();
        if (!radio_net_why[0] || !secure)
            why("send failed");
        shim_log(radio_net_why);
        return -1;
    }
    if (secure)
        shim_log("tls up");
    /* the headers, a line at a time */
    radio_net_name[0] = 0;
    for (;;) {
        unsigned k = 0;
        char c = 0;
        while (k + 1u < sizeof line) {
            if (conn_read(&c, 1) != 1) {
                conn_close();
                if (!radio_net_why[0])
                    why("closed in headers");
                shim_log(radio_net_why);
                return -1;
            }
            if (c == '\n')
                break;
            if (c != '\r')
                line[k++] = c;
        }
        if (c != '\n')                           /* too long: the rest of the line goes */
            while (conn_read(&c, 1) == 1 && c != '\n')
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
            break;                                /* the blank line: the stream follows */
        if (status >= 300 && status < 400 && ieq_prefix(line, "location:")) {
            const char *v = line + 9;
            while (*v == ' ')
                v++;
            strncpy(url, v, URL_MAX - 1);
            url[URL_MAX - 1] = 0;
        } else if (ieq_prefix(line, "icy-name:")) {
            const char *v = line + 9;
            while (*v == ' ')
                v++;
            strncpy(radio_net_name, v, sizeof radio_net_name - 1);
            radio_net_name[sizeof radio_net_name - 1] = 0;
        }
    }
    if (status >= 300 && status < 400) {
        conn_close();
        shim_log("redirect");
        return 1;
    }
    if (status != 200) {
        conn_close();
        snprintf(radio_net_why, sizeof radio_net_why, "http status %d", status);
        shim_log(radio_net_why);
        return -1;
    }
    return 0;
}

static void radio_task(void *arg)
{
    int tries, rc = -1;
    (void)arg;
    running = 1;
    radio_net_state = RN_CONNECTING;
    for (tries = 0; tries < 4 && !stop_req; tries++) {
        rc = open_stream();
        if (rc != 1)
            break;
    }
    if (rc != 0) {
        radio_net_state = RN_ERROR;
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
        got = conn_read(ring + at, n);
        if (got <= 0) {
            if (got == 0 || !tls)
                why(got == 0 ? "stream ended" : "stream stalled");
            else if (!radio_net_why[0])
                why("stream stalled");
            radio_net_state = got == 0 ? RN_ENDED : RN_ERROR;
            break;
        }
        radio_wr += (uint32_t)got;
    }
    conn_close();
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
    return task_create(radio_task, 0, "radio");
}
/* ask the task to stop; it ends after its current recv */
void radio_net_stop(void) { stop_req = 1; }
int radio_net_running(void) { return running; }
