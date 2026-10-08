/*
 * The device without the device: the real state machine, protocol and tag logic, with a
 * simulated NTAG in place of the PN532 and a pseudo-terminal in place of USB.
 *
 * It prints the pty's path on start-up. Point tools/nfcprog.py at it with --port and it
 * behaves as the scanner does, so the harness can be developed and tested with no hardware.
 *
 * Lines beginning with '!' are not protocol; they move the simulated tag:
 *
 *   !tag ntag215        put a new, factory-fresh tag on the reader (also ntag213, ntag216)
 *   !tag classic        put a tag of another kind on the reader
 *   !tap                take the current tag off and put it back
 *   !remove             take it off
 *   !two                two tags in the field at once
 *   !tear N             the tag is pulled away after N more page writes
 *   !nfc off | on       the reader stops or starts answering
 *
 * Each is answered with {"sim":"ok"} or {"sim":"error"}.
 *
 * With SIM_SYNC_URL set (see sim_net.h) it also talks to a plugin, real or fake, over the
 * network link, as the firmware does: the pty is link 0 and the plugin link 1.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_core.h"
#include "proto.h"
#include "sim_ota.h"
#include "sim_net.h"
#include "sim_ntag.h"

#define LINK_PTY    0
#define LINK_NET    1
#define NET_SYNC_CMD_MAX_LINE 2048

static int s_master = -1;
static app_core_t s_core;
static sim_ntag_t s_sim;
static app_tag_t s_tag;
static bool s_tag_on_reader;
static bool s_tag_announced;
static bool s_nfc_ok = true;

static const app_tag_t NTAG = {
    .uid_len = 7, .uid = { 0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 }, .sak = 0x00, .atqa = 0x0044,
};
static const app_tag_t CLASSIC = {
    .uid_len = 4, .uid = { 0xDE, 0xAD, 0xBE, 0xEF }, .sak = 0x08, .atqa = 0x0004,
};

static void write_all(const char *data, size_t len)
{
    while (len > 0) {
        const ssize_t n = write(s_master, data, len);
        if (n > 0) {
            data += n;
            len -= (size_t)n;
        } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
            return;
        } else {
            vTaskDelay(1);
        }
    }
}

static void env_emit(void *ctx, const app_evt_t *evt)
{
    (void)ctx;
    static char line[1024];
    const size_t n = proto_format(evt, line, sizeof(line));
    if (n == 0) {
        return;
    }
    if (evt->origin == LINK_PTY || evt->origin == APP_ORIGIN_ALL) {
        write_all(line, n);
    }
    if (evt->origin == LINK_NET || evt->origin == APP_ORIGIN_ALL) {
        sim_net_queue(line, n);
    }
}

static app_net_status_t s_net_status;

static app_err_t env_net(void *ctx, const app_cmd_t *cmd, app_net_status_t *status, const char **detail)
{
    (void)ctx;
    if (cmd->net_action != APP_NET_STATUS) {
        *detail = "the simulator's network is set by environment variables";
        return APP_ERR_BAD_ARG;
    }
    sim_net_status(status);
    return APP_ERR_NONE;
}

static uint32_t env_now_ms(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
}

static void env_sysinfo(void *ctx, app_sysinfo_t *out)
{
    (void)ctx;
    out->reader = getenv("SIM_READER") ? getenv("SIM_READER") : "nfc-sim000000";
    out->fw = sim_ota_fw();
    out->idf = "host";
    out->pn532_ok = s_nfc_ok;
    out->pn532_ic = 0x32;
    out->pn532_ver = 1;
    out->pn532_rev = 6;
    out->reset = "poweron";
    out->uptime_ms = env_now_ms(NULL);
    if (sim_net_configured()) {
        sim_net_status(&s_net_status);
        out->net = &s_net_status;
    }
}

static void announce_ota(const char *state, const char *detail)
{
    const app_evt_t evt = { .type = APP_EVT_OTA, .origin = APP_ORIGIN_ALL, .state = state, .detail = detail };
    env_emit(NULL, &evt);
}

/* A good image is in: the simulator "restarts" into it, which to a host looks like the
 * firmware's restart: the update says so, and the port says hello with the new version. */
static void sim_restart(const char *new_fw)
{
    (void)new_fw;
    announce_ota("restarting", NULL);
    app_core_link(&s_core, LINK_PTY, false);
    app_core_link(&s_core, LINK_PTY, true);
}

static app_err_t env_ota(void *ctx, const app_cmd_t *cmd, const char **detail)
{
    (void)ctx;
    const bool was_active = sim_ota_active();
    const app_err_t err = sim_ota_command(cmd, env_now_ms(NULL), detail);
    if (err == APP_ERR_NONE && cmd->type == APP_CMD_OTA_BEGIN) {
        announce_ota("downloading", NULL);
    } else if (err != APP_ERR_NONE && was_active && !sim_ota_active()) {
        announce_ota("failed", *detail);
    }
    return err;
}

static bool env_updating(void *ctx)
{
    (void)ctx;
    return sim_ota_active();
}

static void env_hid_type(void *ctx, const char *text)
{
    (void)ctx;
    static char line[512];
    size_t at = (size_t)snprintf(line, sizeof(line), "{\"sim\":\"hid\",\"text\":\"");
    for (; *text && at + 8 < sizeof(line); text++) {
        const unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            line[at++] = '\\';
            line[at++] = (char)c;
        } else if (c < 0x20) {
            at += (size_t)snprintf(line + at, sizeof(line) - at, "\\u%04x", c);
        } else {
            line[at++] = (char)c;
        }
    }
    at += (size_t)snprintf(line + at, sizeof(line) - at, "\"}\n");
    write_all(line, at);
}

static void env_enter_bootloader(void *ctx)
{
    (void)ctx;
    const char *line = "{\"sim\":\"bootloader\"}\n";
    write_all(line, strlen(line));
}

static void put_tag(const app_tag_t *tag, ntag_type_t type)
{
    s_tag = *tag;
    if (type != NTAG_UNKNOWN) {
        sim_ntag_init(&s_sim, type);
    }
    sim_ntag_present(&s_sim);
    s_tag_on_reader = true;
    s_tag_announced = false;
}

static bool control(const char *line)
{
    if (strcmp(line, "!tag ntag213") == 0) {
        put_tag(&NTAG, NTAG_213);
    } else if (strcmp(line, "!tag ntag215") == 0) {
        put_tag(&NTAG, NTAG_215);
    } else if (strcmp(line, "!tag ntag216") == 0) {
        put_tag(&NTAG, NTAG_216);
    } else if (strcmp(line, "!tag classic") == 0) {
        put_tag(&CLASSIC, NTAG_UNKNOWN);
    } else if (strcmp(line, "!tap") == 0) {
        if (s_tag_on_reader) {
            app_core_tag_removed(&s_core, &s_tag);
        }
        put_tag(&s_tag, NTAG_UNKNOWN);
    } else if (strcmp(line, "!remove") == 0) {
        if (s_tag_on_reader) {
            s_tag_on_reader = false;
            s_sim.present = false;
            app_core_tag_removed(&s_core, &s_tag);
        }
    } else if (strcmp(line, "!two") == 0) {
        app_core_tag_conflict(&s_core);
    } else if (strncmp(line, "!tear ", 6) == 0) {
        s_sim.tear_after_writes = atoi(line + 6);
    } else if (strcmp(line, "!nfc off") == 0 || strcmp(line, "!nfc on") == 0) {
        s_nfc_ok = line[6] == 'n';
        app_core_nfc_state(&s_core, s_nfc_ok);
    } else {
        return false;
    }
    return true;
}

/* A command line from link `origin`. */
static void handle_command(uint8_t origin, const char *line, size_t len)
{
    static app_cmd_t cmd;
    proto_err_t err;
    if (proto_parse(line, len, &cmd, &err)) {
        cmd.origin = origin;
        cmd.remote = origin == LINK_NET;
        app_core_command(&s_core, &cmd);
        return;
    }
    app_evt_t evt;
    proto_err_event(&err, &evt);
    evt.origin = origin;
    env_emit(NULL, &evt);
}

static void handle_line(const char *line, size_t len)
{
    if (line[0] == '!') {
        const char *reply = control(line) ? "{\"sim\":\"ok\"}\n" : "{\"sim\":\"error\"}\n";
        write_all(reply, strlen(reply));
        return;
    }
    handle_command(LINK_PTY, line, len);
}

/* What the firmware's poller does: report a tag once when it arrives, and offer it again if
 * a job starts while it is still there. */
static void poll_tag(void)
{
    if (!s_tag_on_reader || !s_nfc_ok) {
        return;
    }
    if (!s_tag_announced || app_core_wants_tag(&s_core)) {
        s_tag_announced = true;
        app_core_tag_arrived(&s_core, &s_tag, sim_ntag_xcvr(&s_sim));
        if (!s_sim.present) {
            /* torn away during the operation */
            s_tag_on_reader = false;
            app_core_tag_removed(&s_core, &s_tag);
        }
    }
}

static int open_pty(void)
{
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        return -1;
    }
    const char *path = ptsname(master);
    if (path == NULL) {
        return -1;
    }

    /* Hold the other end open and raw ourselves. Otherwise the terminal's line discipline
     * echoes what we send back at us until a client arrives to turn that off, and the pty
     * goes away each time a client closes it. */
    const int slave = open(path, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        return -1;
    }
    struct termios tio;
    if (tcgetattr(slave, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(slave, TCSANOW, &tio);
    }
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    printf("PTY %s\n", path);
    fflush(stdout);
    return master;
}

void app_main(void)
{
    s_master = open_pty();
    if (s_master < 0) {
        perror("pty");
        exit(EXIT_FAILURE);
    }

    const app_env_t env = {
        .emit = env_emit,
        .now_ms = env_now_ms,
        .sysinfo = env_sysinfo,
        .hid_type = env_hid_type,
        .enter_bootloader = env_enter_bootloader,
        .net = sim_net_configured() ? env_net : NULL,
        .ota = env_ota,
        .updating = env_updating,
    };
    sim_ota_init(sim_restart);
    app_core_init(&s_core, &env, true);
    sim_net_init();
    app_core_nfc_state(&s_core, true);
    sim_ntag_init(&s_sim, NTAG_215);
    s_sim.present = false;

    static char line[PROTO_LINE_MAX];
    size_t len = 0;
    bool discard = false;

    for (;;) {
        char chunk[256];
        const ssize_t n = read(s_master, chunk, sizeof(chunk));
        for (ssize_t i = 0; i < n; i++) {
            const char c = chunk[i];
            if (c == '\n') {
                if (discard) {
                    discard = false;
                    const char *reply = "{\"evt\":\"error\",\"error\":\"line_too_long\"}\n";
                    write_all(reply, strlen(reply));
                } else {
                    if (len > 0 && line[len - 1] == '\r') {
                        len--;
                    }
                    line[len] = '\0';
                    if (len > 0) {
                        handle_line(line, len);
                    }
                }
                len = 0;
            } else if (!discard) {
                if (len + 1 >= sizeof(line)) {
                    discard = true;
                    len = 0;
                } else {
                    line[len++] = c;
                }
            }
        }
        app_core_tick(&s_core);
        sim_ota_poll(env_now_ms(NULL));
        poll_tag();
        static char net_cmd[NET_SYNC_CMD_MAX_LINE];
        while (sim_net_step(net_cmd, sizeof(net_cmd))) {
            handle_command(LINK_NET, net_cmd, strlen(net_cmd));
        }
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
