/* The radio's network side: one task that fetches an HTTP stream (Icecast / SHOUTcast, plain http) into a
 * ring the app's decoder drains. Built against the SDK's lwIP headers (sockets, DNS); the app sees only
 * radio_net_*. Redirects (3xx Location, http only) are followed a few times. */
#include <stdint.h>
#include <string.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "shim.h"

extern int snprintf(char *buf, unsigned long sz, const char *f, ...);

enum { RN_IDLE, RN_CONNECTING, RN_STREAMING, RN_ERROR, RN_ENDED };
volatile int radio_net_state;
volatile uint32_t radio_rd, radio_wr;            /* the ring: the task writes, the app reads (bytes, ever) */
static uint8_t *ring;
static uint32_t ring_size;
#define URL_MAX 512u                              /* tokenised redirects (zeno.fm) run to ~350 */
static char *url;                                 /* URL_MAX, from the heap once (RAM is full) */
extern void *malloc(unsigned long n);
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

/* "http://host[:port]/path" */
static int split(const char *u, char *host, unsigned hmax, int *port, const char **path)
{
    unsigned n = 0;
    if (strncmp(u, "http://", 7))
        return -1;                                /* https needs TLS: not here */
    u += 7;
    while (*u && *u != '/' && *u != ':' && n + 1u < hmax)
        host[n++] = *u++;
    host[n] = 0;
    *port = 80;
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
static int open_stream(int *sock)
{
    char host[64], req[640], line[URL_MAX];
    const char *path;
    int port, s, n = 0, status = 0;
    struct hostent *he;
    struct sockaddr_in a;
    if (split(url, host, sizeof host, &port, &path)) {
        why("not an http:// url");
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
    n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: FM-1 Net/0.1\r\nIcy-MetaData: 0\r\n"
                 "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
    if (send(s, req, n, 0) != n) {
        closesocket(s);
        why("send failed");
        return -1;
    }
    /* the headers, a line at a time */
    radio_net_name[0] = 0;
    for (;;) {
        unsigned k = 0;
        char c = 0;
        while (k + 1u < sizeof line) {
            if (recv(s, &c, 1, 0) != 1) {
                closesocket(s);
                why("closed in headers");
                return -1;
            }
            if (c == '\n')
                break;
            if (c != '\r')
                line[k++] = c;
        }
        if (c != '\n')                           /* too long: the rest of the line goes */
            while (recv(s, &c, 1, 0) == 1 && c != '\n')
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
        closesocket(s);
        shim_log("redirect");
        return 1;
    }
    if (status != 200) {
        closesocket(s);
        snprintf(radio_net_why, sizeof radio_net_why, "http status %d", status);
        shim_log(radio_net_why);
        return -1;
    }
    *sock = s;
    return 0;
}

static void radio_task(void *arg)
{
    int s = -1, tries, rc = -1;
    (void)arg;
    running = 1;
    radio_net_state = RN_CONNECTING;
    for (tries = 0; tries < 4 && !stop_req; tries++) {
        rc = open_stream(&s);
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
        got = recv(s, ring + at, n, 0);
        if (got <= 0) {
            why(got == 0 ? "stream ended" : "stream stalled");
            radio_net_state = got == 0 ? RN_ENDED : RN_ERROR;
            break;
        }
        radio_wr += (uint32_t)got;
    }
    closesocket(s);
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
