#include "proto.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "ndef.h"

static bool fail(proto_err_t *err, app_err_t code, const char *detail)
{
    err->error = code;
    snprintf(err->detail, sizeof(err->detail), "%s", detail ? detail : "");
    return false;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Decode a hex string into at most `cap` bytes. Returns the byte count, or -1 if it is not
 * hex, has an odd length, or is too long. */
static int hex_decode(const char *s, uint8_t *out, size_t cap)
{
    const size_t n = strlen(s);
    if (n % 2 != 0 || n / 2 > cap) {
        return -1;
    }
    for (size_t i = 0; i < n / 2; i++) {
        const int hi = hex_nibble(s[2 * i]);
        const int lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

/* An optional field holding exactly `len` bytes of hex. *present says whether it was there. */
static bool get_hex_exact(const cJSON *obj, const char *name, uint8_t *out, size_t len,
                          bool *present, proto_err_t *err)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    *present = item != NULL;
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsString(item) || hex_decode(item->valuestring, out, len) != (int)len) {
        char detail[sizeof(err->detail)];
        snprintf(detail, sizeof(detail), "%s: expected %u hex digits", name, (unsigned)(len * 2));
        return fail(err, APP_ERR_BAD_ARG, detail);
    }
    return true;
}

static bool get_bool(const cJSON *obj, const char *name, bool required, bool *out, proto_err_t *err)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (item == NULL && !required) {
        return true;
    }
    if (!cJSON_IsBool(item)) {
        char detail[sizeof(err->detail)];
        snprintf(detail, sizeof(detail), "%s: expected true or false", name);
        return fail(err, APP_ERR_BAD_ARG, detail);
    }
    *out = cJSON_IsTrue(item);
    return true;
}

/* A whole number within [lo, hi]. */
static bool as_integer(const cJSON *item, double lo, double hi, double *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double v = item->valuedouble;
    if (v != floor(v) || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_id(const cJSON *obj, bool required, app_cmd_t *cmd, proto_err_t *err)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (item == NULL) {
        return required ? fail(err, APP_ERR_BAD_ARG, "id: required") : true;
    }
    double v;
    if (!as_integer(item, 0, 2147483647.0, &v)) {
        return fail(err, APP_ERR_BAD_ARG, "id: expected an integer from 0 to 2147483647");
    }
    cmd->has_id = true;
    cmd->id = (int32_t)v;
    err->has_id = true;
    err->id = cmd->id;
    return true;
}

static bool parse_program(const cJSON *obj, app_cmd_t *cmd, proto_err_t *err)
{
    if (!parse_id(obj, true, cmd, err)) {
        return false;
    }

    const cJSON *ndef = cJSON_GetObjectItemCaseSensitive(obj, "ndef");
    if (!cJSON_IsString(ndef)) {
        return fail(err, APP_ERR_BAD_ARG, "ndef: required, as hex");
    }
    const int n = hex_decode(ndef->valuestring, cmd->ndef, sizeof(cmd->ndef));
    if (n < 0) {
        return fail(err, APP_ERR_BAD_ARG, "ndef: not hex, or too long");
    }
    if (!ndef_message_valid(cmd->ndef, (size_t)n)) {
        return fail(err, APP_ERR_BAD_ARG, "ndef: not a complete NDEF message, or text that is not UTF-8");
    }
    cmd->ndef_len = (uint16_t)n;

    bool has_pack = false;
    if (!get_bool(obj, "overwrite", false, &cmd->overwrite, err)
            || !get_hex_exact(obj, "pwd", cmd->pwd, sizeof(cmd->pwd), &cmd->has_pwd, err)
            || !get_hex_exact(obj, "pack", cmd->pack, sizeof(cmd->pack), &has_pack, err)
            || !get_hex_exact(obj, "old_pwd", cmd->old_pwd, sizeof(cmd->old_pwd), &cmd->has_old_pwd, err)) {
        return false;
    }
    if (has_pack && !cmd->has_pwd) {
        return fail(err, APP_ERR_BAD_ARG, "pack: needs pwd");
    }

    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(obj, "timeout_ms");
    if (timeout != NULL) {
        double v;
        if (!as_integer(timeout, APP_JOB_TIMEOUT_MIN_MS, APP_JOB_TIMEOUT_MAX_MS, &v)) {
            return fail(err, APP_ERR_BAD_ARG, "timeout_ms: expected 1000 to 600000");
        }
        cmd->timeout_ms = (uint32_t)v;
    }
    return true;
}

static bool parse_wipe(const cJSON *obj, app_cmd_t *cmd, proto_err_t *err)
{
    if (!parse_id(obj, true, cmd, err)
            || !get_hex_exact(obj, "pwd", cmd->pwd, sizeof(cmd->pwd), &cmd->has_pwd, err)) {
        return false;
    }
    const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(obj, "timeout_ms");
    if (timeout != NULL) {
        double v;
        if (!as_integer(timeout, APP_JOB_TIMEOUT_MIN_MS, APP_JOB_TIMEOUT_MAX_MS, &v)) {
            return fail(err, APP_ERR_BAD_ARG, "timeout_ms: expected 1000 to 600000");
        }
        cmd->timeout_ms = (uint32_t)v;
    }
    return true;
}

static bool parse_log(const cJSON *obj, app_cmd_t *cmd, proto_err_t *err)
{
    static const struct {
        const char *name;
        app_log_level_t level;
    } levels[] = {
        { "off", APP_LOG_OFF }, { "error", APP_LOG_ERROR }, { "warn", APP_LOG_WARN },
        { "info", APP_LOG_INFO }, { "debug", APP_LOG_DEBUG },
    };
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(obj, "level");
    if (cJSON_IsString(level)) {
        for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
            if (strcmp(level->valuestring, levels[i].name) == 0) {
                cmd->level = levels[i].level;
                return true;
            }
        }
    }
    return fail(err, APP_ERR_BAD_ARG, "level: expected off, error, warn, info or debug");
}

/* An optional string of at most `cap - 1` bytes. *present says whether it was there. */
static bool get_string(const cJSON *obj, const char *name, char *out, size_t cap, bool *present, proto_err_t *err)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    *present = item != NULL;
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= cap) {
        char detail[sizeof(err->detail)];
        snprintf(detail, sizeof(detail), "%s: expected a string of up to %u characters", name, (unsigned)(cap - 1));
        return fail(err, APP_ERR_BAD_ARG, detail);
    }
    strcpy(out, item->valuestring);
    return true;
}

static bool get_uint(const cJSON *obj, const char *name, double lo, double hi, uint32_t *out, bool *present, proto_err_t *err)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    *present = item != NULL;
    if (item == NULL) {
        return true;
    }
    double v;
    if (!as_integer(item, lo, hi, &v)) {
        char detail[sizeof(err->detail)];
        snprintf(detail, sizeof(detail), "%s: expected %.0f to %.0f", name, lo, hi);
        return fail(err, APP_ERR_BAD_ARG, detail);
    }
    *out = (uint32_t)v;
    return true;
}

static bool looks_like_url(const char *s)
{
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

/*
 * {"cmd":"net"}                                                   report
 * {"cmd":"net","action":"join","ssid":"...","psk":"..."}          remember and join
 * {"cmd":"net","action":"forget","ssid":"..."}
 * {"cmd":"net","action":"server","url":"https://...","token":"..."}
 * {"cmd":"net","action":"poll","poll_ms":1000,"wait_s":25}
 * {"cmd":"net","enabled":false}                                   with or without an action
 */
static bool parse_net(const cJSON *obj, app_cmd_t *cmd, proto_err_t *err)
{
    bool has_ssid, has_psk, has_url, has_action;
    char action[16];
    if (!get_string(obj, "action", action, sizeof(action), &has_action, err)
            || !get_string(obj, "ssid", cmd->ssid, sizeof(cmd->ssid), &has_ssid, err)
            || !get_string(obj, "psk", cmd->psk, sizeof(cmd->psk), &has_psk, err)
            || !get_string(obj, "url", cmd->url, sizeof(cmd->url), &has_url, err)
            || !get_string(obj, "token", cmd->token, sizeof(cmd->token), &cmd->has_token, err)
            || !get_uint(obj, "poll_ms", 100, 60000, &cmd->poll_ms, &cmd->has_poll_ms, err)
            || !get_uint(obj, "wait_s", 0, 300, &cmd->wait_s, &cmd->has_wait_s, err)) {
        return false;
    }
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(obj, "enabled");
    cmd->has_enabled = enabled != NULL;
    if (cmd->has_enabled && !get_bool(obj, "enabled", true, &cmd->enabled, err)) {
        return false;
    }

    if (!has_action) {
        cmd->net_action = APP_NET_STATUS;
        return true;
    }
    if (strcmp(action, "join") == 0) {
        cmd->net_action = APP_NET_JOIN;
        if (!has_ssid || cmd->ssid[0] == '\0') {
            return fail(err, APP_ERR_BAD_ARG, "ssid: required");
        }
        const size_t n = strlen(cmd->psk);
        if (n != 0 && n < 8) {
            return fail(err, APP_ERR_BAD_ARG, "psk: 8 to 64 characters, or none for an open network");
        }
        return true;
    }
    if (strcmp(action, "forget") == 0) {
        cmd->net_action = APP_NET_FORGET;
        if (!has_ssid || cmd->ssid[0] == '\0') {
            return fail(err, APP_ERR_BAD_ARG, "ssid: required");
        }
        return true;
    }
    if (strcmp(action, "server") == 0) {
        cmd->net_action = APP_NET_SERVER;
        if (!has_url || !looks_like_url(cmd->url)) {
            return fail(err, APP_ERR_BAD_ARG, "url: required, beginning http:// or https://");
        }
        return true;
    }
    if (strcmp(action, "poll") == 0) {
        cmd->net_action = APP_NET_POLL;
        if (!cmd->has_poll_ms && !cmd->has_wait_s) {
            return fail(err, APP_ERR_BAD_ARG, "poll: give poll_ms, wait_s or both");
        }
        return true;
    }
    return fail(err, APP_ERR_BAD_ARG, "action: expected join, forget, server or poll");
}

/* {"cmd":"ota","url":"https://...","sha256":"<64 hex>"}: both required. */
static bool parse_ota(const cJSON *obj, app_cmd_t *cmd, proto_err_t *err)
{
    bool has_url;
    if (!get_string(obj, "url", cmd->ota_url, sizeof(cmd->ota_url), &has_url, err)) {
        return false;
    }
    if (!has_url || !looks_like_url(cmd->ota_url)) {
        return fail(err, APP_ERR_BAD_ARG, "url: required, beginning http:// or https://");
    }
    if (!get_hex_exact(obj, "sha256", cmd->sha256, sizeof(cmd->sha256), &cmd->has_sha256, err)) {
        return false;
    }
    return cmd->has_sha256 ? true : fail(err, APP_ERR_BAD_ARG, "sha256: required, 64 hex digits");
}

static bool parse_debug(const cJSON *obj, app_cmd_t *cmd, proto_err_t *err)
{
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(obj, "action");
    if (cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "crash") == 0) {
            cmd->debug = APP_DEBUG_CRASH;
            return true;
        }
        if (strcmp(action->valuestring, "hang") == 0) {
            cmd->debug = APP_DEBUG_HANG;
            return true;
        }
        if (strcmp(action->valuestring, "nousb") == 0) {
            cmd->debug = APP_DEBUG_NOUSB;
            return true;
        }
    }
    return fail(err, APP_ERR_BAD_ARG, "action: expected crash, hang or nousb");
}

bool proto_parse(const char *line, size_t len, app_cmd_t *cmd, proto_err_t *err)
{
    memset(err, 0, sizeof(*err));
    memset(cmd, 0, sizeof(*cmd));
    cmd->timeout_ms = APP_JOB_TIMEOUT_DEFAULT_MS;

    cJSON *obj = cJSON_ParseWithLength(line, len);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        return fail(err, APP_ERR_BAD_JSON, "expected one JSON object");
    }

    bool ok;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(obj, "cmd");
    if (!cJSON_IsString(name)) {
        ok = fail(err, APP_ERR_BAD_JSON, "cmd: required");
    } else if (strcmp(name->valuestring, "info") == 0) {
        cmd->type = APP_CMD_INFO;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_id(obj, false, cmd, err);
    } else if (strcmp(name->valuestring, "program") == 0) {
        cmd->type = APP_CMD_PROGRAM;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_program(obj, cmd, err);
    } else if (strcmp(name->valuestring, "wipe") == 0) {
        cmd->type = APP_CMD_WIPE;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_wipe(obj, cmd, err);
    } else if (strcmp(name->valuestring, "cancel") == 0) {
        cmd->type = APP_CMD_CANCEL;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_id(obj, false, cmd, err);
    } else if (strcmp(name->valuestring, "hid") == 0) {
        cmd->type = APP_CMD_HID;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_id(obj, false, cmd, err) && get_bool(obj, "enabled", true, &cmd->enabled, err)
             && get_bool(obj, "persist", false, &cmd->persist, err);
    } else if (strcmp(name->valuestring, "log") == 0) {
        cmd->type = APP_CMD_LOG;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_id(obj, false, cmd, err) && parse_log(obj, cmd, err);
    } else if (strcmp(name->valuestring, "bootloader") == 0) {
        cmd->type = APP_CMD_BOOTLOADER;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_id(obj, false, cmd, err);
    } else if (strcmp(name->valuestring, "debug") == 0) {
        cmd->type = APP_CMD_DEBUG;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_id(obj, false, cmd, err) && parse_debug(obj, cmd, err);
    } else if (strcmp(name->valuestring, "net") == 0) {
        cmd->type = APP_CMD_NET;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_id(obj, false, cmd, err) && parse_net(obj, cmd, err);
    } else if (strcmp(name->valuestring, "ota") == 0) {
        cmd->type = APP_CMD_OTA;
        err->cmd = app_cmd_name(cmd->type);
        ok = parse_id(obj, false, cmd, err) && parse_ota(obj, cmd, err);
    } else {
        /* Answered as a `rsp` under the name given, so the sender can match it up. A long name
         * is cut, on a character boundary so the answer is still a string. */
        snprintf(err->name, sizeof(err->name), "%s", name->valuestring);
        for (size_t i = strlen(err->name); i > 0 && (err->name[i - 1] & 0xC0) == 0x80; i--) {
            err->name[i - 1] = '\0';
        }
        if (strlen(err->name) == sizeof(err->name) - 1 && (err->name[sizeof(err->name) - 2] & 0x80)) {
            err->name[sizeof(err->name) - 2] = '\0';       /* the lead byte of a cut character */
        }
        err->cmd = err->name;
        ok = parse_id(obj, false, cmd, err) && fail(err, APP_ERR_UNKNOWN_CMD, "");
    }

    cJSON_Delete(obj);
    return ok;
}

void proto_err_event(const proto_err_t *err, app_evt_t *evt)
{
    memset(evt, 0, sizeof(*evt));
    evt->error = err->error;
    evt->detail = err->detail[0] ? err->detail : NULL;
    if (err->cmd) {
        evt->type = APP_EVT_RSP;
        evt->cmd = err->cmd;
        evt->ok = false;
        evt->has_id = err->has_id;
        evt->id = err->id;
    } else {
        evt->type = APP_EVT_ERROR;
    }
}

static void uid_hex(const app_evt_t *evt, char *hex)
{
    for (size_t i = 0; i < evt->uid_len && i < APP_UID_MAX; i++) {
        snprintf(hex + 2 * i, 3, "%02X", evt->uid[i]);
    }
}

static void add_uid(cJSON *obj, const app_evt_t *evt)
{
    if (evt->uid_len == 0) {
        return;
    }
    char hex[2 * APP_UID_MAX + 1];
    uid_hex(evt, hex);
    cJSON_AddStringToObject(obj, "uid", hex);
}

static void add_id(cJSON *obj, const app_evt_t *evt)
{
    if (evt->has_id) {
        cJSON_AddNumberToObject(obj, "id", evt->id);
    }
}

static void add_string(cJSON *obj, const char *name, const char *value)
{
    if (value) {
        cJSON_AddStringToObject(obj, name, value);
    }
}

static void add_string_or_null(cJSON *obj, const char *name, const char *value)
{
    if (value) {
        cJSON_AddStringToObject(obj, name, value);
    } else {
        cJSON_AddNullToObject(obj, name);
    }
}

/* The network side, flattened into `obj`. A passphrase and the token are never reported. */
static void add_net(cJSON *obj, const app_net_status_t *net)
{
    cJSON_AddBoolToObject(obj, "enabled", net->enabled);
    add_string(obj, "wifi", net->wifi);
    add_string_or_null(obj, "ssid", net->ssid);
    add_string_or_null(obj, "ip", net->ip);
    add_string_or_null(obj, "url", net->url);
    cJSON_AddBoolToObject(obj, "token", net->has_token);
    add_string(obj, "reader", net->reader);
    add_string(obj, "link", net->link);
    cJSON_AddNumberToObject(obj, "last_status", net->last_status);
    cJSON_AddNumberToObject(obj, "poll_ms", net->poll_ms);
    cJSON_AddNumberToObject(obj, "wait_s", net->wait_s);
    cJSON_AddNumberToObject(obj, "queued", net->queued);
    cJSON_AddNumberToObject(obj, "dropped", net->dropped);
}

static void add_info(cJSON *obj, const app_evt_t *evt)
{
    const app_sysinfo_t *sys = evt->sys;
    cJSON_AddNumberToObject(obj, "proto", APP_PROTO_VERSION);
    add_string(obj, "fw", sys->fw);
    add_string(obj, "idf", sys->idf);
    if (sys->pn532_ok) {
        char ver[12];
        snprintf(ver, sizeof(ver), "%u.%u", sys->pn532_ver, sys->pn532_rev);
        cJSON *pn532 = cJSON_AddObjectToObject(obj, "pn532");
        cJSON_AddNumberToObject(pn532, "ic", sys->pn532_ic);
        cJSON_AddStringToObject(pn532, "ver", ver);
    } else {
        cJSON_AddNullToObject(obj, "pn532");
    }
    add_string(obj, "state", evt->state);
    if (evt->has_job) {
        cJSON_AddNumberToObject(obj, "job", evt->job_id);
    } else {
        cJSON_AddNullToObject(obj, "job");
    }
    /* The tag on the reader now, so a page can tell before it starts a job. */
    if (evt->uid_len) {
        char hex[2 * APP_UID_MAX + 1];
        uid_hex(evt, hex);
        cJSON_AddStringToObject(obj, "tag", hex);
    } else {
        cJSON_AddNullToObject(obj, "tag");
    }
    cJSON_AddBoolToObject(obj, "hid", evt->hid);
    cJSON_AddBoolToObject(obj, "buzzer", sys->buzzer);
    add_string(obj, "reset", sys->reset);
    add_string_or_null(obj, "crash", sys->crash);
    cJSON_AddNumberToObject(obj, "uptime_ms", sys->uptime_ms);
    if (sys->net) {
        add_net(cJSON_AddObjectToObject(obj, "net"), sys->net);
    } else {
        cJSON_AddNullToObject(obj, "net");
    }
}

static void add_error(cJSON *obj, const app_evt_t *evt)
{
    if (evt->error != APP_ERR_NONE) {
        cJSON_AddStringToObject(obj, "error", app_err_name(evt->error));
    }
    add_string(obj, "detail", evt->detail);
}

static bool put(char *out, size_t cap, size_t *at, const char *s, size_t n)
{
    if (*at + n >= cap) {               /* always room left for the terminator */
        return false;
    }
    memcpy(out + *at, s, n);
    *at += n;
    out[*at] = '\0';
    return true;
}

static bool put_str(char *out, size_t cap, size_t *at, const char *s)
{
    return put(out, cap, at, s, strlen(s));
}

/* `s` as the inside of a JSON string. */
static bool put_escaped(char *out, size_t cap, size_t *at, const char *s)
{
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        char esc[8];
        switch (c) {
        case '"':  snprintf(esc, sizeof(esc), "\\\""); break;
        case '\\': snprintf(esc, sizeof(esc), "\\\\"); break;
        case '\b': snprintf(esc, sizeof(esc), "\\b"); break;
        case '\f': snprintf(esc, sizeof(esc), "\\f"); break;
        case '\n': snprintf(esc, sizeof(esc), "\\n"); break;
        case '\r': snprintf(esc, sizeof(esc), "\\r"); break;
        case '\t': snprintf(esc, sizeof(esc), "\\t"); break;
        default:
            if (c < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", c);
            } else {
                esc[0] = (char)c;
                esc[1] = '\0';
            }
            break;
        }
        if (!put_str(out, cap, at, esc)) {
            return false;
        }
    }
    return true;
}

/*
 * A log line, built without the heap: this runs inside the log hook, on whichever task is
 * logging, and must not allocate or block.
 */
static size_t format_log(const app_evt_t *evt, char *out, size_t cap)
{
    const char lvl[2] = { evt->lvl ? evt->lvl : '?', '\0' };
    size_t at = 0;
    const bool ok = put_str(out, cap, &at, "{\"evt\":\"log\",\"lvl\":\"")
                    && put_escaped(out, cap, &at, lvl)
                    && put_str(out, cap, &at, "\"")
                    && (!evt->log_tag || (put_str(out, cap, &at, ",\"tag\":\"")
                                          && put_escaped(out, cap, &at, evt->log_tag)
                                          && put_str(out, cap, &at, "\"")))
                    && (!evt->msg || (put_str(out, cap, &at, ",\"msg\":\"")
                                      && put_escaped(out, cap, &at, evt->msg)
                                      && put_str(out, cap, &at, "\"")))
                    && put_str(out, cap, &at, "}\n");
    return ok ? at : 0;
}

size_t proto_format(const app_evt_t *evt, char *out, size_t cap)
{
    if (evt->type == APP_EVT_LOG) {
        return format_log(evt, out, cap);
    }

    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return 0;
    }

    switch (evt->type) {
    case APP_EVT_RSP:
        cJSON_AddStringToObject(obj, "rsp", evt->cmd ? evt->cmd : "");
        cJSON_AddBoolToObject(obj, "ok", evt->ok);
        add_id(obj, evt);
        add_error(obj, evt);
        if (evt->sys) {
            add_info(obj, evt);
        } else if (evt->has_hid) {
            cJSON_AddBoolToObject(obj, "enabled", evt->hid);
        } else if (evt->net) {
            add_net(obj, evt->net);
        }
        break;
    case APP_EVT_HELLO:
        cJSON_AddStringToObject(obj, "evt", "hello");
        cJSON_AddNumberToObject(obj, "proto", APP_PROTO_VERSION);
        if (evt->sys) {
            add_string(obj, "fw", evt->sys->fw);
        }
        break;
    case APP_EVT_WAITING:
        cJSON_AddStringToObject(obj, "evt", "waiting");
        add_id(obj, evt);
        cJSON_AddNumberToObject(obj, "timeout_ms", evt->timeout_ms);
        break;
    case APP_EVT_WRITING:
        cJSON_AddStringToObject(obj, "evt", "writing");
        add_id(obj, evt);
        add_uid(obj, evt);
        break;
    case APP_EVT_DONE:
        cJSON_AddStringToObject(obj, "evt", "done");
        add_id(obj, evt);
        add_uid(obj, evt);
        add_string(obj, "type", evt->tag_type);
        if (evt->has_protected) {
            cJSON_AddBoolToObject(obj, "protected", evt->is_protected);
        }
        break;
    case APP_EVT_FAILED:
        cJSON_AddStringToObject(obj, "evt", "failed");
        add_id(obj, evt);
        add_error(obj, evt);
        add_uid(obj, evt);
        add_string(obj, "text", evt->text);
        add_string(obj, "uri", evt->uri);
        break;
    case APP_EVT_TAG:
        cJSON_AddStringToObject(obj, "evt", "tag");
        add_uid(obj, evt);
        add_string(obj, "type", evt->tag_type);
        add_string(obj, "text", evt->text);
        add_string(obj, "uri", evt->uri);
        if (evt->has_protected) {
            cJSON_AddBoolToObject(obj, "protected", evt->is_protected);
        }
        add_error(obj, evt);
        break;
    case APP_EVT_TAG_REMOVED:
        cJSON_AddStringToObject(obj, "evt", "tag_removed");
        add_uid(obj, evt);
        break;
    case APP_EVT_ERROR:
        cJSON_AddStringToObject(obj, "evt", "error");
        add_error(obj, evt);
        break;
    case APP_EVT_NET:
        cJSON_AddStringToObject(obj, "evt", "net");
        if (evt->net) {
            add_net(obj, evt->net);
        }
        break;
    case APP_EVT_OTA:
        cJSON_AddStringToObject(obj, "evt", "ota");
        add_string(obj, "state", evt->state);
        add_error(obj, evt);
        break;
    case APP_EVT_LOG:
        break;                          /* handled above */
    }

    /* cJSON wants a few bytes of slack beyond what it prints; one more is kept for the '\n'.
     * A tag's text and uri, once escaped, can outgrow any line: the event then goes without
     * them, since its outcome matters more than what the tag held. */
    size_t len = 0;
    for (int attempt = 0; attempt < 2 && len == 0; attempt++) {
        if (cap > 1 && cJSON_PrintPreallocated(obj, out, (int)(cap - 1), false)) {
            len = strlen(out);
            out[len++] = '\n';
            out[len] = '\0';
        } else if (cJSON_HasObjectItem(obj, "text") || cJSON_HasObjectItem(obj, "uri")) {
            cJSON_DeleteItemFromObject(obj, "text");
            cJSON_DeleteItemFromObject(obj, "uri");
            cJSON_DeleteItemFromObject(obj, "detail");
            cJSON_AddStringToObject(obj, "detail", "text and uri omitted: too long for a line");
        } else {
            break;
        }
    }
    cJSON_Delete(obj);
    return len;
}
