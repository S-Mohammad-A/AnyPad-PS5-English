#include "web.h"
#include "log.h"
#include "util.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WEB_CLIENTS     8
#define REQ_MAX         8192
#define T_REQUEST       3000    /* ms for a client to send its request */
#define LOG_TAIL        6144

extern const unsigned char web_page_html[];
extern const unsigned int web_page_html_len;

typedef struct {
    int fd;
    int local;                          /* the peer is on the local network */
    long t_start;
    int len;
    char buf[REQ_MAX + 1];
} client;

struct web {
    web_cfg cfg;
    int fd;
    client c[WEB_CLIENTS];
};

static int g_allow_any_peer;            /* tests only */

/* ---- helpers ------------------------------------------------------------- */

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* Sends all of it, giving up on a client too slow to take it. */
static void send_all(int fd, const char *p, size_t n)
{
    long deadline = now_ms() + 1000;

    while (n > 0) {
        ssize_t k = send(fd, p, n, 0);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
        } else if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && now_ms() < deadline) {
            usleep(1000);
        } else {
            return;
        }
    }
}

static void respond(int fd, int code, const char *type, const char *body, size_t len)
{
    char head[256];
    const char *msg = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 429 ? "Too Many Requests" : "Error";
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                     "Connection: close\r\n\r\n",
                     code, msg, type, (unsigned)len);
    send_all(fd, head, (size_t)n);
    send_all(fd, body, len);
}

static void respond_text(int fd, int code, const char *text)
{
    respond(fd, code, "application/json", text, strlen(text));
}

/* The value of a header, name compared without regard to case. */
static int header(const char *req, const char *name, char *out, size_t max)
{
    size_t nl = strlen(name);
    const char *p = strstr(req, "\r\n");

    while (p && p[2] != '\r') {
        const char *line = p + 2, *end = strstr(line, "\r\n");
        if (!end) return 0;
        if ((size_t)(end - line) > nl && line[nl] == ':' && strncasecmp(line, name, nl) == 0) {
            const char *v = line + nl + 1;
            size_t k = 0;
            while (*v == ' ') v++;
            while (v < end && k + 1 < max) out[k++] = *v++;
            out[k] = '\0';
            return 1;
        }
        p = end;
    }
    return 0;
}

/* A Host that is an IPv4 address (or localhost), with an optional port: not
 * a name. A web page on the Internet cannot make a browser use one to reach
 * the console (DNS rebinding hands the browser the console's address under
 * the attacker's name, and the name is what arrives here). */
static int host_is_literal(const char *h)
{
    unsigned a, b, c, d;
    char tail;
    int n;

    if (strncmp(h, "localhost", 9) == 0 && (h[9] == '\0' || h[9] == ':')) return 1;
    n = sscanf(h, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &tail);
    if (n == 4) return a < 256 && b < 256 && c < 256 && d < 256;
    return n == 5 && tail == ':' && a < 256 && b < 256 && c < 256 && d < 256;
}

/* Loopback or a private range: the page is for the local network only. */
static int peer_is_local(const struct sockaddr_in *sin)
{
    uint32_t a = ntohl(sin->sin_addr.s_addr);

    return (a >> 24) == 127 || (a >> 24) == 10 || (a >> 20) == 0xAC1 ||
           (a >> 16) == 0xC0A8 || (a >> 16) == 0xA9FE;
}

static int parse_addr(const char *s, unsigned char a[6])
{
    unsigned v[6];
    int i;

    if (sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return 0;
    for (i = 0; i < 6; i++) a[5 - i] = (unsigned char)v[i];   /* shown MSB first */
    return 1;
}

static void fmt_addr(char *out, const unsigned char *a)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* ---- API --------------------------------------------------------------------- */

/* Copies `s` into out as JSON string content: quotes, backslashes and
 * control characters are dropped, so nothing can break the document. */
static void json_text(char *out, size_t max, const char *s)
{
    size_t n = 0;

    for (; s && *s && n + 1 < max; s++)
        if ((unsigned char)*s >= 0x20 && *s != '"' && *s != '\\') out[n++] = *s;
    out[n] = '\0';
}

static void api_state(web_t *w, int fd)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        { PAD_CROSS, "cross" }, { PAD_CIRCLE, "circle" }, { PAD_SQUARE, "square" },
        { PAD_TRIANGLE, "triangle" }, { PAD_L1, "l1" }, { PAD_R1, "r1" }, { PAD_L2, "l2" },
        { PAD_R2, "r2" }, { PAD_L3, "l3" }, { PAD_R3, "r3" }, { PAD_UP, "up" },
        { PAD_DOWN, "down" }, { PAD_LEFT, "left" }, { PAD_RIGHT, "right" },
        { PAD_OPTIONS, "options" }, { PAD_CREATE, "create" }, { PAD_PS, "ps" },
        { PAD_TOUCHPAD, "touchpad" },
    };
    host_t *h = *w->cfg.host;
    char out[8192], a[18];
    size_t n = 0;
    int i, first = 1;

#define PUT(...) do { int k_ = snprintf(out + n, sizeof out - n, __VA_ARGS__); \
                      if (k_ > 0) n += (size_t)k_ < sizeof out - n ? (size_t)k_ : sizeof out - n - 1; } while (0)

    {
        static const char *const bt[] = { "trying", "ok", "failed" };
        char reason[200], combo[100];
        uint32_t pb = 0;
        int perr = 0, readable = w->cfg.physical ? w->cfg.physical(&pb, &perr) : 0;
        int st = *w->cfg.bt_state;
        size_t b;
        int fb = 1;

        json_text(reason, sizeof reason, w->cfg.bt_reason);
        json_text(combo, sizeof combo, w->cfg.combo_text);
        PUT("{\"version\":\"%s\",\"running\":%s,\"pairing\":%d,\"bluetooth\":\"%s\","
            "\"reason\":\"%s\",\"combo\":\"%s\",\"physical\":{\"readable\":%s,\"error\":\"%08x\",\"buttons\":[",
            w->cfg.version, h ? "true" : "false", h ? host_pairing_left(h) : 0,
            bt[st >= 0 && st <= 2 ? st : 0], reason, combo, readable ? "true" : "false",
            (unsigned)perr);
        for (b = 0; readable && b < sizeof names / sizeof names[0]; b++)
            if (pb & names[b].bit) {
                PUT("%s\"%s\"", fb ? "" : ",", names[b].name);
                fb = 0;
            }
        PUT("]},\"pads\":[");
    }
    for (i = 0; h && i < HOST_MAX_PADS; i++) {
        pad_info info;
        pad_state st;
        size_t b;
        int fb = 1;
        if (!host_pad_get(h, i, &info, &st)) continue;
        fmt_addr(a, info.addr);
        PUT("%s{\"slot\":%d,\"profile\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"addr\":\"%s\","
            "\"battery\":%d,\"charging\":%s,\"lx\":%u,\"ly\":%u,\"rx\":%u,\"ry\":%u,\"l2\":%u,\"r2\":%u,"
            "\"buttons\":[",
            first ? "" : ",", i + 1, info.profile, info.vid, info.pid, a,
            st.has_battery ? st.battery_pct : -1, st.charging ? "true" : "false",
            st.lx, st.ly, st.rx, st.ry, st.l2, st.r2);
        for (b = 0; b < sizeof names / sizeof names[0]; b++)
            if (st.buttons & names[b].bit) {
                PUT("%s\"%s\"", fb ? "" : ",", names[b].name);
                fb = 0;
            }
        PUT("]}");
        first = 0;
    }
    PUT("],\"paired\":[");
    first = 1;
    for (i = 0; h && i < host_paired_count(h); i++) {
        host_paired_info p;
        if (!host_paired_get(h, i, &p)) break;
        fmt_addr(a, p.addr);
        PUT("%s{\"addr\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"le\":%s,\"connected\":%s}",
            first ? "" : ",", a, p.vid, p.pid, p.le ? "true" : "false",
            p.connected ? "true" : "false");
        first = 0;
    }
    PUT("]}");
#undef PUT
    respond(fd, 200, "application/json", out, n);
}

/* The end of the log, as plain text with anything unprintable dropped. */
static void api_log(web_t *w, int fd)
{
    char buf[LOG_TAIL];
    FILE *f = fopen(w->cfg.log_path, "rb");
    size_t n = 0, i;
    long size;

    if (!f) {
        respond(fd, 200, "text/plain; charset=utf-8", "", 0);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, size > LOG_TAIL ? size - LOG_TAIL : 0, SEEK_SET);
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    for (i = 0; i < n; i++)
        if ((unsigned char)buf[i] < 0x20 && buf[i] != '\n') buf[i] = ' ';
    respond(fd, 200, "text/plain; charset=utf-8", buf, n);
}

static void handle(web_t *w, int fd, char *req)
{
    char method[8], path[256], hostname[64], custom[8];
    host_t *h = *w->cfg.host;

    if (sscanf(req, "%7s %255s", method, path) != 2) {
        respond_text(fd, 400, "{\"error\":\"bad request\"}");
        return;
    }
    if (!header(req, "Host", hostname, sizeof hostname) || !host_is_literal(hostname)) {
        respond_text(fd, 400, "{\"error\":\"host\"}");
        return;
    }
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        respond(fd, 200, "text/html; charset=utf-8", (const char *)web_page_html, web_page_html_len);
        return;
    }
    if (strncmp(path, "/api/", 5) != 0) {
        respond_text(fd, 404, "{\"error\":\"not found\"}");
        return;
    }

    /* Anything that changes something must carry a header only this page
     * sends: a browser will not add it to a request made by another site
     * without asking us first, and we never agree. */
    if (strcmp(method, "POST") == 0 &&
        (!header(req, "X-AnyPad", custom, sizeof custom) || strcmp(custom, "1") != 0)) {
        respond_text(fd, 403, "{\"error\":\"forbidden\"}");
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/state") == 0) {
        api_state(w, fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/log") == 0) {
        api_log(w, fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/pair") == 0 && h) {
        host_pair(h, 60);
        log_line("web: pairing started");
        respond_text(fd, 200, "{\"ok\":true}");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/forget?addr=", 17) == 0 && h) {
        unsigned char a[6];
        if (!parse_addr(path + 17, a)) respond_text(fd, 400, "{\"error\":\"address\"}");
        else if (!host_forget(h, a)) respond_text(fd, 404, "{\"error\":\"not paired\"}");
        else respond_text(fd, 200, "{\"ok\":true}");
    } else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/ps?slot=", 13) == 0) {
        int n = atoi(path + 13);
        if (n < 1 || n > HOST_MAX_PADS) respond_text(fd, 400, "{\"error\":\"slot\"}");
        else if (!w->cfg.press_ps || !w->cfg.press_ps(n - 1)) respond_text(fd, 404, "{\"error\":\"no pad\"}");
        else respond_text(fd, 200, "{\"ok\":true}");
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/retry") == 0) {
        if (*w->cfg.bt_state == WEB_BT_FAILED) {
            log_line("web: Bluetooth retry requested");
            *w->cfg.retry = 1;
            respond_text(fd, 200, "{\"ok\":true}");
        } else {
            respond_text(fd, 400, "{\"error\":\"not failed\"}");
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/stop") == 0) {
        log_line("web: stop requested");
        *w->cfg.stop = 1;
        respond_text(fd, 200, "{\"ok\":true}");
    } else {
        respond_text(fd, 404, "{\"error\":\"not found\"}");
    }
}

/* ---- serving ------------------------------------------------------------------- */

web_t *web_start(const web_cfg *cfg, int port)
{
    struct sockaddr_in sin;
    web_t *w;
    int one = 1, i;

    w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->cfg = *cfg;
    for (i = 0; i < WEB_CLIENTS; i++) w->c[i].fd = -1;

    w->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (w->fd < 0) {
        free(w);
        return NULL;
    }
    setsockopt(w->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(w->fd, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(w->fd, 4) != 0) {
        log_line("web: port %d not available (errno %d), no web page", port, errno);
        close(w->fd);
        free(w);
        return NULL;
    }
    set_nonblock(w->fd);
    log_line("web: listening on port %d", port);
    return w;
}

void web_poll(web_t *w)
{
    long now = now_ms();
    int i;

    if (!w) return;
    for (;;) {                          /* new connections */
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int fd = accept(w->fd, (struct sockaddr *)&peer, &plen), slot = -1;
        if (fd < 0) break;
        if (plen < sizeof peer || (!g_allow_any_peer && !peer_is_local(&peer))) {
            close(fd);                  /* not from the local network */
            continue;
        }
#ifdef SO_NOSIGPIPE
        {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        }
#endif
        set_nonblock(fd);
        for (i = 0; i < WEB_CLIENTS; i++) if (w->c[i].fd < 0) { slot = i; break; }
        if (slot < 0) {
            respond_text(fd, 429, "{\"error\":\"busy\"}");
            close(fd);
            continue;
        }
        w->c[slot].fd = fd;
        w->c[slot].len = 0;
        w->c[slot].t_start = now;
    }

    for (i = 0; i < WEB_CLIENTS; i++) {
        client *c = &w->c[i];
        ssize_t k;

        if (c->fd < 0) continue;
        k = recv(c->fd, c->buf + c->len, (size_t)(REQ_MAX - c->len), 0);
        if (k > 0) c->len += (int)k;
        c->buf[c->len] = '\0';

        if (strstr(c->buf, "\r\n\r\n")) {
            handle(w, c->fd, c->buf);
        } else if (k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            /* the client went away */
        } else if (c->len >= REQ_MAX) {
            respond_text(c->fd, 400, "{\"error\":\"too large\"}");
        } else if (now - c->t_start <= T_REQUEST) {
            continue;                   /* still coming */
        }
        close(c->fd);
        c->fd = -1;
    }
}

void web_stop(web_t **owner)
{
    web_t *w;
    int i;

    if (!owner || !*owner) return;
    w = *owner;
    *owner = NULL;                     /* idle callbacks must not reuse this server */
    for (i = 0; i < WEB_CLIENTS; i++) if (w->c[i].fd >= 0) close(w->c[i].fd);
    close(w->fd);
    free(w);
}
