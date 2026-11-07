/* The web page against a simulated DualShock 4 whose buttons and sticks
 * move, to look at in a browser on port 18096. Stops after two minutes. */
#include "sim.h"
#include "../src/host.h"
#include "../src/web.h"
#include "../src/util.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    static const unsigned char addr[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
    volatile int stop = 0;
    host_t *h;
    web_t *w;
    sim_pad pad;
    sim_t *sim;
    web_cfg wc;
    long i;

    signal(SIGPIPE, SIG_IGN);
    unlink("build/demo.db");
    memset(&pad, 0, sizeof pad);
    pad.mode = SIM_WAIT_PAIRING;
    memcpy(pad.addr, addr, 6);
    pad.cod = 0x002508;
    pad.vid = 0x054C;
    pad.pid = 0x05C4;
    memset(pad.report, 0, 78);
    pad.report[0] = 0x11;
    pad.report_len = 78;
    pad.needs_output = 1;
    sim = sim_new(&pad);
    h = host_open(sim_hci(sim), "build/demo.db", NULL);
    {
        static volatile int bt = WEB_BT_OK, retry;
        memset(&wc, 0, sizeof wc);
        wc.host = &h;
        wc.stop = &stop;
        wc.retry = &retry;
        wc.bt_state = &bt;
        wc.bt_reason = "";
        wc.log_path = "build/demo.log";
        wc.version = "demo";
        wc.combo_text = "L2 + R2 + OPTIONS";
    }
    w = web_start(&wc, 18096);
    if (!h || !w) return 1;
    host_pair(h, 30);
    for (i = 0; i < 24000 && !stop; i++) {
        double t = i / 200.0;
        pad.report[3] = (unsigned char)(128 + 100 * __builtin_sin(t));
        pad.report[4] = (unsigned char)(128 + 100 * __builtin_cos(t));
        pad.report[5] = (unsigned char)(128 + 60 * __builtin_cos(t * 1.7));
        pad.report[6] = (unsigned char)(128 - 60 * __builtin_sin(t * 1.3));
        pad.report[7] = (unsigned char)(((i / 400) % 2 ? 0x20 : 0x40) | 0x08);
        pad.report[8] = (i / 600) % 2 ? 0x01 : 0x00;
        pad.report[10] = (unsigned char)((i % 400) * 255 / 400);
        pad.report[32] = 0x06;
        sim_set_report(sim, pad.report, 78);
        host_poll(h, 5);
        web_poll(w);
        usleep(5000);
    }
    web_stop(&w);
    host_close(h);
    return 0;
}
