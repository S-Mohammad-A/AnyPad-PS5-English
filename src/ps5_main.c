/* AnyPad PS5 -- Bluetooth game controllers on the PlayStation 5.
 *
 * Pads from other consoles and brands connect to the console's own
 * Bluetooth chip, beside the DualSense, and appear to games as DualSense
 * controllers of the main user.
 *
 *   hci_usb.c   the Bluetooth chip, shared with the system's driver
 *   host.c      pairing, reconnection, L2CAP, SDP and HID (Classic)
 *   le.c        the same over Bluetooth LE
 *   profiles.c  each controller's reports
 *   ps5_vpad.c  the virtual DualSense each pad drives
 *   web.c       the menu: a page on port 8095
 *   (no console hotkey: a plain payload cannot read the DualSense; the menu opens from the media row)
 *
 * ALPHA: see PRUEBAS.md for what has been tried on a console.
 *
 * Stability comes first. It only touches what it created: its own Bluetooth
 * links, its own virtual pads, its own files under /data/anypad. When
 * something it depends on fails, it says so (log, notification, the menu)
 * and stays out of the way instead of retrying forever.
 *
 * The menu is started first, before anything that can fail, so that it is
 * there to explain a failure. It is a web page: from a phone or a PC at the
 * address the start-up notification gives, or on the console itself by
 * holding a button combination on the DualSense (L2 + R2 + OPTIONS for 1.5
 * seconds unless /data/anypad/config.ini says otherwise).
 *
 * Stopping: the menu's button, or create /data/anypad/stop. It disconnects
 * its pads, removes its virtual pads, puts back the controller setting it
 * changed, and exits. It does the same by itself when the console starts
 * going to rest mode (read from SceSystemStateMgrInfo), so nothing of it is left half-way across a sleep.
 * Pairing: the menu's button, or create /data/anypad/pair.
 */
#include "hci_usb.h"
#include "host.h"
#include "lock.h"
#include "log.h"
#include "netinfo.h"
#include "ps5_power.h"
#include "ps5_ui.h"
#include "ps5_apps.h"
#include "ps5_sysinfo.h"
#include "ps5_vpad.h"
#include "util.h"
#include "version.h"
#include "web.h"

#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIR    "/data/anypad"
#define LOG_PATH     STATE_DIR "/anypad.log"
#define DB_PATH      STATE_DIR "/pads.db"
#define LOCK_PATH    STATE_DIR "/anypad.lock"
#define CONFIG_PATH  STATE_DIR "/config.ini"
#define PAIR_FLAG    STATE_DIR "/pair"
#define STOP_FLAG    STATE_DIR "/stop"
#define NO_ICON_FLAG STATE_DIR "/no_icon"          /* do not put AnyPad in the media row */
#define RM_ICON_FLAG STATE_DIR "/remove_icon"      /* take it out, once */
#define PAIR_SECONDS 60

#define OPEN_TRIES      5       /* opening Bluetooth: 5 tries, 3 s apart */
#define RAISED_TRIES    2       /* ... the first two with raised credentials */

/* Findable in the binary: strings AnyPad-PS5-*.elf | grep anypad-version */
static const char g_version_tag[] __attribute__((used)) = "anypad-version " ANYPAD_VERSION;

static volatile sig_atomic_t g_stop;
static volatile int g_web_stop;         /* the menu's stop button */
static volatile int g_retry;            /* the menu's retry button */
static volatile int g_bt_state = WEB_BT_TRYING;
static char g_bt_reason[200];

static host_t *g_host;                  /* NULL until Bluetooth is up */
static web_t *g_web;
static int g_vpad_ok;                   /* virtual pads are possible */
static int g_bt_attempt;                /* Bluetooth tries made in this round */
static long g_bt_next;                  /* when the next one is due */
static int g_bt_plain_creds;            /* the chip answered only without raised credentials */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---- virtual pads follow the controllers ---------------------------------- */

static void on_connect(void *ud, int slot, const pad_info *info)
{
    (void)ud;
    if (vpad_add(slot)) notify("AnyPad PS5: %s conectado", info->profile);
    else notify("AnyPad PS5: %s conectado, pero sin mando virtual (ver el log)", info->profile);
}

static void on_state(void *ud, int slot, const pad_state *st)
{
    (void)ud;
    vpad_update(slot, st);
}

/* The menu's PS button: the same as holding PS on that pad. */
#define PS_HOLD_MS 700
static int web_press_ps(int slot)
{
    return g_vpad_ok && vpad_press_ps(slot, PS_HOLD_MS);
}

static void on_disconnect(void *ud, int slot)
{
    (void)ud;
    vpad_remove(slot);
    notify("AnyPad PS5: mando %d desconectado", slot + 1);
}

/* ---- Bluetooth, one try at a time ------------------------------------------ */

/* The host calls this whenever it waits for the chip, so the menu answers
 * even while a Bluetooth try is in progress (it used to freeze for ~5 s each
 * time, and the browser's pile-up of connections got "busy"). */
static void on_idle(void *ud)
{
    (void)ud;
    web_poll(g_web);
}

static void bt_begin(void)
{
    g_bt_attempt = 0;
    g_bt_next = 0;
    g_bt_reason[0] = '\0';
    g_bt_state = WEB_BT_TRYING;
    if (g_vpad_ok) vpad_creds_raise();
}

/* One try. The controller may answer only to a process that does not carry
 * raised credentials (not known), so the first
 * tries are made as they are and the rest with the original ones. Which
 * worked is logged. Virtual pads raise them again when they need to. */
static void bt_try(long now)
{
    host_events ev = { on_connect, on_state, on_disconnect, NULL, on_idle };
    hci_t hci;
    int attempt = ++g_bt_attempt;

    if (attempt == RAISED_TRIES + 1 && g_vpad_ok) {
        log_line("Bluetooth silent with raised credentials; trying with the original ones");
        if (vpad_creds_restore()) g_bt_plain_creds = 1;
    }
    if (hci_usb_open(&hci)) {
        g_host = host_open(hci, DB_PATH, &ev);
        if (g_host) {
            g_bt_state = WEB_BT_OK;
            log_line("Bluetooth ready after %d tr%s%s", attempt, attempt == 1 ? "y" : "ies",
                     g_bt_plain_creds ? ", answering only with the original credentials" : "");
            notify("AnyPad PS5: Bluetooth listo (%d mandos emparejados)", host_paired_count(g_host));
            return;
        }
        hci.ops->close(hci.ctx);
    }
    log_line("Bluetooth not ready (try %d of %d)", attempt, OPEN_TRIES);
    if (attempt >= OPEN_TRIES) {
        g_bt_state = WEB_BT_FAILED;
        snprintf(g_bt_reason, sizeof g_bt_reason,
                 "El chip Bluetooth no responde (%d intentos, con y sin credenciales elevadas). "
                 "Detalle en el log, lineas 'diag'.", attempt);
        log_line("Bluetooth unavailable: %s", g_bt_reason);
        notify("AnyPad PS5: el Bluetooth no responde.\nSigo activo: abre el menu para ver el motivo");
        return;
    }
    g_bt_next = now + 3000;
}

static void bt_lost(const char *why)
{
    log_line("Bluetooth lost: %s", why);
    host_close(g_host);                 /* drops every pad and its virtual pad */
    g_host = NULL;
    g_bt_state = WEB_BT_FAILED;
    snprintf(g_bt_reason, sizeof g_bt_reason, "Se perdio el Bluetooth (%s). Pulsa Reintentar.", why);
    notify("AnyPad PS5: se perdio el Bluetooth.\nPulsa Reintentar en el menu");
}

/* ---- flags ---------------------------------------------------------------- */

static void check_flags(long now)
{
    static long last_second;

    if (now / 1000 == last_second) return;
    last_second = now / 1000;
    if (access(STOP_FLAG, F_OK) == 0) {
        unlink(STOP_FLAG);
        g_stop = 1;
    }
    if (g_host && access(PAIR_FLAG, F_OK) == 0) {
        unlink(PAIR_FLAG);
        host_pair(g_host, PAIR_SECONDS);
        notify("AnyPad PS5: emparejando %d s. Pon el mando en modo emparejamiento", PAIR_SECONDS);
    }
}

int main(void)
{
    web_cfg wc;
    char ip[16], url[64];
    const char *why = "stop requested";
    long last_power = 0;

    mkdir(STATE_DIR, 0755);
    log_open(LOG_PATH);
    log_line("AnyPad PS5 %s", ANYPAD_VERSION);
    (void)g_version_tag;

    if (!lock_take(LOCK_PATH)) {
        log_line("another instance is running");
        notify("AnyPad PS5: ya esta en marcha");
        log_close();
        return 1;
    }
    unlink(STOP_FLAG);                  /* an old request is not for us */
    syscall(SYS_thr_set_name, -1, "anypad");    /* how others find us */
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);           /* a browser closing early */


    /* The menu first: it is what explains everything that follows. */
    memset(&wc, 0, sizeof wc);
    wc.host = &g_host;
    wc.stop = &g_web_stop;
    wc.retry = &g_retry;
    wc.bt_state = &g_bt_state;
    wc.bt_reason = g_bt_reason;
    wc.log_path = LOG_PATH;
    wc.version = ANYPAD_VERSION;
    wc.press_ps = web_press_ps;
    g_web = web_start(&wc, WEB_PORT);

    local_ip(ip);
    snprintf(url, sizeof url, "http://%s:%d/", ip, WEB_PORT);
    if (g_web)
        notify("AnyPad PS5 %s\nMenu: %s (o Contenido multimedia > AnyPad)\nAntes crea un usuario nuevo y asignale el mando cuando la consola lo pregunte. Juega con tu usuario y pulsa PS un momento para usar ese mando", ANYPAD_VERSION, url);
    else
        notify("AnyPad PS5 %s\nNo se pudo abrir la pagina del menu (puerto %d ocupado?)",
               ANYPAD_VERSION, WEB_PORT);

    sysinfo_log();

    /* The entry in the media row that opens the menu in the browser. Done
     * before the credentials are raised for the pads, as Payload Manager does
     * it with its own; /data/anypad/no_icon skips it, remove_icon undoes it. */
    if (access(RM_ICON_FLAG, F_OK) == 0) {
        apps_remove_launcher();
        unlink(RM_ICON_FLAG);
    } else if (access(NO_ICON_FLAG, F_OK) != 0) {
        apps_install_launcher();
    }

    /* Without virtual pads there is nothing to connect controllers to:
     * Bluetooth is not touched at all, and the menu says why. */
    g_vpad_ok = vpad_init();
    if (!g_vpad_ok) {
        g_bt_state = WEB_BT_FAILED;
        snprintf(g_bt_reason, sizeof g_bt_reason,
                 "No se pueden crear mandos virtuales en este sistema (ver el log). No se ha tocado el Bluetooth.");
        notify("AnyPad PS5: no se pueden crear mandos virtuales.\nVer el menu o el log. El Bluetooth no se ha tocado");
    } else {
        bt_begin();
    }

    while (!g_stop && !g_web_stop) {
        long now;

        if (g_host) host_poll(g_host, 4);
        else usleep(4000);
        now = now_ms();
        web_poll(g_web);
        if (g_vpad_ok) vpad_poll(now);

        /* Rest mode on its way: leave cleanly before it, not after. */
        if (now - last_power >= 200) {
            int ps = power_state();
            last_power = now;
            if (ps == POWER_GOING_TO_REST || ps == POWER_IN_REST) {
                why = "rest mode";
                notify("AnyPad PS5: parado por el modo reposo.\nLanzalo de nuevo al despertar");
                break;
            }
        }

        /* The Bluetooth: tries, loss, and the menu's retry button. */
        if (g_bt_state == WEB_BT_TRYING && now >= g_bt_next) bt_try(now);
        if (g_host && host_transport_lost(g_host)) bt_lost("sin respuesta del chip");
        if (g_retry) {
            g_retry = 0;
            if (g_vpad_ok && g_bt_state == WEB_BT_FAILED) {
                log_line("retrying Bluetooth at the user's request");
                bt_begin();
            } else {
                log_line("retry ignored (virtual pads not available, or not failed)");
            }
        }

        check_flags(now);
    }
    if (g_web_stop) why = "stop from the menu";
    else if (g_stop && strcmp(why, "stop requested") == 0) why = "stop requested (flag or signal)";

    log_line("stopping: %s", why);
    web_stop(&g_web);
    host_close(g_host);                 /* our links; removes our virtual pads */
    g_host = NULL;
    if (g_vpad_ok) {
        vpad_creds_restore();           /* leave the credentials as found */
    }
    lock_release();
    log_line("stopped");
    log_close();
    notify("AnyPad PS5: parado");
    return 0;
}
