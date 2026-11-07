/* The web page's server against a host with a simulated pad connected. */
#include "sim.h"
#include "../src/host.h"
#include "../src/web.h"
#include "../src/util.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* A port per run, so two runs at once do not collide. */
static int g_port;
#define PORT g_port

static host_t *g_host;
static web_t *g_web;
static int g_idle_calls;

/* Match the payload: Bluetooth waits also service the web server. */
static void on_idle(void *ud)
{
    (void)ud;
    g_idle_calls++;
    web_poll(g_web);
}
static volatile int g_bt_state = WEB_BT_OK, g_retry;
static char g_reason[200] = "";
static int g_phys_readable = 1;

static int fake_physical(uint32_t *buttons, int *err)
{
    *err = g_phys_readable ? 0 : (int)0xFFFF0003;
    *buttons = PAD_L1 | PAD_R1;
    return g_phys_readable;
}

static int g_ps_calls, g_ps_slot = -1, g_ps_result = 1;

static int fake_press_ps(int slot)
{
    g_ps_calls++;
    g_ps_slot = slot;
    return g_ps_result;
}

static void run(int ms)
{
    long end = sim_now() + ms;
    while (sim_now() < end) {
        host_poll(g_host, 5);
        web_poll(g_web);
    }
}

/* Sends a raw request and serves it; returns the response (status line on). */
static int request(const char *raw, size_t rawlen, char *out, int max)
{
    struct sockaddr_in sin;
    int fd = socket(AF_INET, SOCK_STREAM, 0), n = 0, i;

    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(PORT);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sin, sizeof sin) != 0) {
        close(fd);
        return -1;
    }
    if (rawlen) send(fd, raw, rawlen, 0);
    for (i = 0; i < 400; i++) {             /* the server runs in this thread */
        ssize_t k;
        web_poll(g_web);
        if (g_host) host_poll(g_host, 1);
        k = recv(fd, out + n, (size_t)(max - 1 - n), MSG_DONTWAIT);
        if (k > 0) n += (int)k;
        else if (k == 0) break;
        usleep(1000);
    }
    out[n] = '\0';
    close(fd);
    return n;
}

static int get(const char *method, const char *path, char *out, int max)
{
    char raw[512];
    int n = snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: 192.168.1.50:8095\r\nX-AnyPad: 1\r\n\r\n",
                     method, path);
    return request(raw, (size_t)n, out, max);
}

static int get_with(const char *head, char *out, int max)
{
    return request(head, strlen(head), out, max);
}

int main(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    static char out[70000];
    volatile int stop = 0;
    sim_pad pad;
    sim_t *sim;
    web_cfg wc;
    host_events ev = { 0 };
    int i, idle_before_close;

    signal(SIGPIPE, SIG_IGN);
    g_port = 20000 + (int)(getpid() % 20000);
    unlink("build/test_web.db");
    memset(&pad, 0, sizeof pad);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    memset(pad.report, 0, 78);
    pad.report[0] = 0x11;
    pad.report[3] = 0x80; pad.report[4] = 0x80; pad.report[5] = 0x80; pad.report[6] = 0x80;
    pad.report[7] = 0x20 | 0x08;            /* cross, hat centred */
    pad.report[32] = 0x08;
    pad.report_len = 78;
    pad.needs_output = 1;
    sim = sim_new(&pad);
    ev.idle = on_idle;
    g_host = host_open(sim_hci(sim), "build/test_web.db", &ev);
    memset(&wc, 0, sizeof wc);
    wc.host = &g_host;
    wc.stop = &stop;
    wc.retry = &g_retry;
    wc.bt_state = &g_bt_state;
    wc.bt_reason = g_reason;
    wc.log_path = "build/test_web.log";
    wc.version = "test";
    wc.combo_text = "L2 + R2 + OPTIONS";
    wc.physical = fake_physical;
    wc.press_ps = fake_press_ps;
    g_web = web_start(&wc, PORT);
    CHECK(g_host && g_web);
    if (!g_host || !g_web) return 1;
    {
        FILE *f = fopen("build/test_web.log", "w");
        if (f) { fputs("line one\nline two\n", f); fclose(f); }
    }

    printf("the page\n");
    get("GET", "/", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 200", 12) == 0);
    CHECK(strstr(out, "<title>AnyPad PS5</title>") != NULL);

    printf("pairing from the page, then the pad in the state\n");
    get("POST", "/api/pair", out, sizeof out);
    CHECK(strstr(out, "{\"ok\":true}") != NULL);
    run(8000);
    get("GET", "/api/state", out, sizeof out);
    CHECK(strstr(out, "\"profile\":\"DualShock 4\"") != NULL);
    CHECK(strstr(out, "\"addr\":\"06:05:04:03:02:01\"") != NULL);
    CHECK(strstr(out, "\"buttons\":[\"cross\"]") != NULL);
    CHECK(strstr(out, "\"battery\":100") != NULL);
    CHECK(strstr(out, "\"connected\":true") != NULL);

    CHECK(strstr(out, "\"bluetooth\":\"ok\"") != NULL);
    CHECK(strstr(out, "\"combo\":\"L2 + R2 + OPTIONS\"") != NULL);
    CHECK(strstr(out, "\"physical\":{\"readable\":true,\"error\":\"00000000\",\"buttons\":[\"l1\",\"r1\"]}") != NULL);

    printf("the user's pad not readable (the system has the input)\n");
    g_phys_readable = 0;
    get("GET", "/api/state", out, sizeof out);
    CHECK(strstr(out, "\"readable\":false,\"error\":\"ffff0003\",\"buttons\":[]") != NULL);
    g_phys_readable = 1;

    printf("no Bluetooth: the page still answers, says why, and can retry\n");
    {
        host_t *saved = g_host;
        g_host = NULL;
        g_bt_state = WEB_BT_FAILED;
        snprintf(g_reason, sizeof g_reason, "no reply \"0x1005\" \\ after 5 tries\n");
        get("GET", "/api/state", out, sizeof out);
        CHECK(strncmp(out, "HTTP/1.1 200", 12) == 0);
        CHECK(strstr(out, "\"running\":false") != NULL && strstr(out, "\"bluetooth\":\"failed\"") != NULL);
        CHECK(strstr(out, "\"reason\":\"no reply 0x1005  after 5 tries\"") != NULL);   /* nothing that breaks JSON */
        CHECK(strstr(out, "\"pads\":[]") != NULL && strstr(out, "\"paired\":[]") != NULL);
        get("POST", "/api/pair", out, sizeof out);                  /* nothing to pair with */
        CHECK(strncmp(out, "HTTP/1.1 404", 12) == 0);
        CHECK(g_retry == 0);
        get("POST", "/api/retry", out, sizeof out);
        CHECK(strstr(out, "{\"ok\":true}") != NULL && g_retry == 1);
        g_retry = 0;
        g_bt_state = WEB_BT_OK;
        get("POST", "/api/retry", out, sizeof out);                 /* not failed: refused */
        CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0 && g_retry == 0);
        g_host = saved;
    }

    printf("the log tail\n");
    get("GET", "/api/log", out, sizeof out);
    CHECK(strstr(out, "line two") != NULL);

    printf("the PS button of a pad\n");
    get("POST", "/api/ps?slot=1", out, sizeof out);
    CHECK(strstr(out, "{\"ok\":true}") != NULL && g_ps_calls == 1 && g_ps_slot == 0);   /* slots are 1-based in the page */
    get("POST", "/api/ps?slot=9", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0 && g_ps_calls == 1);
    get("POST", "/api/ps?slot=0", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0 && g_ps_calls == 1);
    g_ps_result = 0;                                    /* no live virtual pad there */
    get("POST", "/api/ps?slot=2", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 404", 12) == 0 && g_ps_calls == 2);
    g_ps_result = 1;
    get_with("POST /api/ps?slot=1 HTTP/1.1\r\nHost: 10.0.0.1\r\n\r\n", out, sizeof out);   /* no X-AnyPad header */
    CHECK(strncmp(out, "HTTP/1.1 403", 12) == 0 && g_ps_calls == 2);

    printf("forgetting the pad\n");
    get("POST", "/api/forget?addr=zz", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);
    get("POST", "/api/forget?addr=06:05:04:03:02:01", out, sizeof out);
    CHECK(strstr(out, "{\"ok\":true}") != NULL);
    run(300);
    CHECK(host_paired_count(g_host) == 0);
    get("GET", "/api/state", out, sizeof out);
    CHECK(strstr(out, "\"pads\":[]") != NULL && strstr(out, "\"paired\":[]") != NULL);

    printf("bad requests: unknown, garbage, oversized, silent, many at once\n");
    get("GET", "/nothing", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 404", 12) == 0);
    {
        static const char junk[] = "\x01\x02\x03 garbage\r\n\r\n";
        request(junk, sizeof junk - 1, out, sizeof out);
    }
    CHECK(strncmp(out, "HTTP/1.1 4", 10) == 0);
    {
        static char big[10000];
        memset(big, 'A', sizeof big);
        request(big, sizeof big, out, sizeof out);
        CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);
    }
    for (i = 0; i < 10; i++) request("GET / HTTP/1.1\r\n", 16, out, sizeof out);  /* never finished */
    get("GET", "/", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 200", 12) == 0);         /* still serving */

    printf("defences that need no PIN\n");
    /* a POST without the page's own header, as a forged cross-site form would send */
    get_with("POST /api/stop HTTP/1.1\r\nHost: 192.168.1.50:8095\r\n\r\n", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 403", 12) == 0 && stop == 0);
    /* DNS rebinding: the browser's Host is the attacker's name */
    get_with("GET /api/state HTTP/1.1\r\nHost: evil.example.com\r\nX-AnyPad: 1\r\n\r\n", out, sizeof out);
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);
    get_with("GET / HTTP/1.1\r\n\r\n", out, sizeof out);          /* no Host at all */
    CHECK(strncmp(out, "HTTP/1.1 400", 12) == 0);
    get_with("OPTIONS /api/stop HTTP/1.1\r\nHost: 192.168.1.50\r\nOrigin: http://evil.example.com\r\n"
             "Access-Control-Request-Method: POST\r\n\r\n", out, sizeof out);
    CHECK(strstr(out, "Access-Control-Allow") == NULL);     /* a preflight is never agreed to */
    CHECK(stop == 0);

    printf("random and mutated requests (under the sanitizers when run with make fuzz)\n");
    {
        static const char *seeds[] = {
            "GET /api/state HTTP/1.1\r\nHost: 10.0.0.1\r\nX-AnyPad: 1\r\n\r\n",
            "POST /api/forget?addr=06:05:04:03:02:01 HTTP/1.1\r\nHost: 10.0.0.1\r\nX-AnyPad: 1\r\n\r\n",
            "GET /api/log HTTP/1.1\r\nHost: localhost:8095\r\n\r\n",
        };
        unsigned seed = 99;
        int round;
        for (round = 0; round < 600; round++) {
            char req[400];
            size_t n;
            int k, m;
            seed = seed * 1103515245u + 12345u;
            snprintf(req, sizeof req, "%s", seeds[(seed >> 8) % 3]);
            n = strlen(req);
            m = (int)((seed >> 12) % 6);
            for (k = 0; k < m; k++) {
                seed = seed * 1103515245u + 12345u;
                req[(seed >> 8) % n] = (char)(seed >> 20);
            }
            if (round % 5 == 0) n = (seed >> 6) % n;            /* truncated */
            request(req, n, out, sizeof out);
        }
        get("GET", "/api/state", out, sizeof out);
        CHECK(strncmp(out, "HTTP/1.1 200", 12) == 0);           /* still alive and well */
    }

    printf("shutdown with a connected pad and a web-serving idle hook\n");
    get("POST", "/api/pair", out, sizeof out);
    run(8000);
    CHECK(host_connected(g_host, 0));

    printf("stop from the page\n");
    get("POST", "/api/stop", out, sizeof out);
    CHECK(stop == 1);

    web_stop(&g_web);
    CHECK(g_web == NULL);
    web_stop(&g_web);                   /* already stopped */
    web_stop(NULL);
    idle_before_close = g_idle_calls;
    host_close(g_host);
    g_host = NULL;
    CHECK(g_idle_calls > idle_before_close); /* cleanup really called web_poll(NULL) */
    sim_free(sim);
    printf(g_fail ? "%d check(s) failed\n" : "all web checks passed\n", g_fail);
    return g_fail != 0;
}
