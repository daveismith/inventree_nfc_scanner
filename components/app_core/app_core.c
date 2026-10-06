#include "app_core.h"

#include <string.h>

const char *app_err_name(app_err_t err)
{
    switch (err) {
    case APP_ERR_NONE:           return "none";
    case APP_ERR_BAD_JSON:       return "bad_json";
    case APP_ERR_LINE_TOO_LONG:  return "line_too_long";
    case APP_ERR_UNKNOWN_CMD:    return "unknown_cmd";
    case APP_ERR_BAD_ARG:        return "bad_arg";
    case APP_ERR_BUSY:           return "busy";
    case APP_ERR_NO_JOB:         return "no_job";
    case APP_ERR_TIMEOUT:        return "timeout";
    case APP_ERR_CANCELLED:      return "cancelled";
    case APP_ERR_WRONG_TAG_TYPE: return "wrong_tag_type";
    case APP_ERR_MULTIPLE_TAGS:  return "multiple_tags";
    case APP_ERR_NOT_BLANK:      return "not_blank";
    case APP_ERR_AUTH_REQUIRED:  return "auth_required";
    case APP_ERR_AUTH_FAILED:    return "auth_failed";
    case APP_ERR_LOCKED:         return "locked";
    case APP_ERR_TOO_LARGE:      return "too_large";
    case APP_ERR_TAG_REMOVED:    return "tag_removed";
    case APP_ERR_WRITE_FAILED:   return "write_failed";
    case APP_ERR_VERIFY_FAILED:  return "verify_failed";
    case APP_ERR_NFC_ERROR:      return "nfc_error";
    case APP_ERR_NOT_ALLOWED:    return "not_allowed";
    }
    return "unknown";
}

const char *app_cmd_name(app_cmd_type_t type)
{
    switch (type) {
    case APP_CMD_INFO:       return "info";
    case APP_CMD_PROGRAM:    return "program";
    case APP_CMD_WIPE:       return "wipe";
    case APP_CMD_CANCEL:     return "cancel";
    case APP_CMD_HID:        return "hid";
    case APP_CMD_LOG:        return "log";
    case APP_CMD_BOOTLOADER: return "bootloader";
    case APP_CMD_DEBUG:      return "debug";
    case APP_CMD_NET:        return "net";
    case APP_CMD_OTA:        return "ota";
    }
    return "unknown";
}

static app_err_t err_of_ntag(ntag_err_t err)
{
    switch (err) {
    case NTAG_OK:                return APP_ERR_NONE;
    case NTAG_ERR_REMOVED:       return APP_ERR_TAG_REMOVED;
    case NTAG_ERR_IO:            return APP_ERR_NFC_ERROR;
    case NTAG_ERR_WRONG_TYPE:    return APP_ERR_WRONG_TAG_TYPE;
    case NTAG_ERR_NOT_BLANK:     return APP_ERR_NOT_BLANK;
    case NTAG_ERR_AUTH_REQUIRED: return APP_ERR_AUTH_REQUIRED;
    case NTAG_ERR_AUTH_FAILED:   return APP_ERR_AUTH_FAILED;
    case NTAG_ERR_LOCKED:        return APP_ERR_LOCKED;
    case NTAG_ERR_TOO_LARGE:     return APP_ERR_TOO_LARGE;
    case NTAG_ERR_WRITE:         return APP_ERR_WRITE_FAILED;
    case NTAG_ERR_VERIFY:        return APP_ERR_VERIFY_FAILED;
    }
    return APP_ERR_NFC_ERROR;
}

static void emit(app_core_t *core, const app_evt_t *evt)
{
    core->env.emit(core->env.ctx, evt);
}

/* An event every link gets. */
static void broadcast(app_core_t *core, app_evt_t *evt)
{
    evt->origin = APP_ORIGIN_ALL;
    emit(core, evt);
}

static void feedback(app_core_t *core, app_feedback_t fb)
{
    if (core->env.feedback) {
        core->env.feedback(core->env.ctx, fb);
    }
}

static void set_uid(app_evt_t *evt, const app_tag_t *tag)
{
    evt->uid_len = tag->uid_len <= APP_UID_MAX ? tag->uid_len : APP_UID_MAX;
    memcpy(evt->uid, tag->uid, evt->uid_len);
}

/* The answer to a command: ok, or not and why. */
static void respond_detail(app_core_t *core, const app_cmd_t *cmd, app_err_t error, const char *detail)
{
    app_evt_t evt = {
        .type = APP_EVT_RSP,
        .origin = cmd->origin,
        .cmd = app_cmd_name(cmd->type),
        .ok = error == APP_ERR_NONE,
        .error = error,
        .detail = detail,
        .has_id = cmd->has_id,
        .id = cmd->id,
    };
    emit(core, &evt);
}

static void respond(app_core_t *core, const app_cmd_t *cmd, app_err_t error)
{
    respond_detail(core, cmd, error, NULL);
}

bool app_core_hid_enabled(const app_core_t *core)
{
    return core->hid_override_set ? core->hid_override : core->hid_default;
}

const char *app_core_state_name(const app_core_t *core)
{
    if (!core->nfc_ok) {
        return "nfc_error";
    }
    return core->job_active ? "waiting" : "idle";
}

/* What the light shows when nothing momentary is happening. */
static void show_resting_state(app_core_t *core)
{
    if (!core->nfc_ok) {
        feedback(core, APP_FB_NFC_ERROR);
    } else {
        feedback(core, core->job_active ? APP_FB_JOB_WAITING : APP_FB_IDLE);
    }
}

void app_core_init(app_core_t *core, const app_env_t *env, bool hid_default)
{
    memset(core, 0, sizeof(*core));
    core->env = *env;
    core->hid_default = hid_default;
    core->auth0 = NTAG_AUTH0_DEFAULT;
}

/* End the job with `failed`. `evt` carries whatever is known beyond the id and the error. */
static void job_failed(app_core_t *core, app_err_t error, app_evt_t *evt)
{
    app_evt_t blank = { 0 };
    if (evt == NULL) {
        evt = &blank;
    }
    evt->type = APP_EVT_FAILED;
    evt->error = error;
    evt->has_id = true;
    evt->id = core->job.id;
    core->job_active = false;
    broadcast(core, evt);
    feedback(core, APP_FB_JOB_FAILED);
    show_resting_state(core);
}

static void cmd_info(app_core_t *core, const app_cmd_t *cmd)
{
    app_sysinfo_t sys = { 0 };
    core->env.sysinfo(core->env.ctx, &sys);
    app_evt_t evt = {
        .type = APP_EVT_RSP,
        .origin = cmd->origin,
        .cmd = app_cmd_name(cmd->type),
        .ok = true,
        .sys = &sys,
        .state = app_core_state_name(core),
        .has_job = core->job_active,
        .job_id = core->job.id,
        .has_hid = true,
        .hid = app_core_hid_enabled(core),
    };
    if (core->tag_here) {
        set_uid(&evt, &core->here);
    }
    emit(core, &evt);
}

static void cmd_job(app_core_t *core, const app_cmd_t *cmd)
{
    if (!core->nfc_ok) {
        respond(core, cmd, APP_ERR_NFC_ERROR);
        return;
    }
    if (core->job_active) {
        respond(core, cmd, APP_ERR_BUSY);
        return;
    }
    core->job = *cmd;
    core->job_active = true;
    core->job_deadline = core->env.now_ms(core->env.ctx) + cmd->timeout_ms;
    respond(core, cmd, APP_ERR_NONE);

    app_evt_t evt = {
        .type = APP_EVT_WAITING,
        .has_id = true,
        .id = cmd->id,
        .timeout_ms = cmd->timeout_ms,
    };
    broadcast(core, &evt);
    show_resting_state(core);
}

static void cmd_cancel(app_core_t *core, const app_cmd_t *cmd)
{
    if (!core->job_active || (cmd->has_id && cmd->id != core->job.id)) {
        respond(core, cmd, APP_ERR_NO_JOB);
        return;
    }
    respond(core, cmd, APP_ERR_NONE);
    job_failed(core, APP_ERR_CANCELLED, NULL);
}

static void cmd_hid(app_core_t *core, const app_cmd_t *cmd)
{
    if (cmd->persist) {
        core->hid_default = cmd->enabled;
        core->hid_override_set = false;
        if (core->env.hid_save_default) {
            core->env.hid_save_default(core->env.ctx, cmd->enabled);
        }
    } else {
        core->hid_override_set = true;
        core->hid_override = cmd->enabled;
        core->hid_override_origin = cmd->origin;
    }
    app_evt_t evt = {
        .type = APP_EVT_RSP,
        .origin = cmd->origin,
        .cmd = app_cmd_name(cmd->type),
        .ok = true,
        .has_hid = true,
        .hid = app_core_hid_enabled(core),
    };
    emit(core, &evt);
}

static void cmd_net(app_core_t *core, const app_cmd_t *cmd)
{
    app_net_status_t status = { 0 };
    const char *detail = NULL;
    const app_err_t err = core->env.net(core->env.ctx, cmd, &status, &detail);
    if (err != APP_ERR_NONE) {
        respond_detail(core, cmd, err, detail);
        return;
    }
    app_evt_t evt = {
        .type = APP_EVT_RSP,
        .origin = cmd->origin,
        .cmd = app_cmd_name(cmd->type),
        .ok = true,
        .net = &status,
    };
    emit(core, &evt);
}

/* What a link that is not in hand may not do: anything that needs someone at the board to
 * undo it, and anything that changes how the device is reached. */
static bool needs_hands(app_cmd_type_t type)
{
    return type == APP_CMD_BOOTLOADER || type == APP_CMD_DEBUG || type == APP_CMD_NET;
}

void app_core_command(app_core_t *core, const app_cmd_t *cmd)
{
    if (cmd->remote && needs_hands(cmd->type)) {
        respond(core, cmd, APP_ERR_NOT_ALLOWED);
        return;
    }
    switch (cmd->type) {
    case APP_CMD_INFO:
        cmd_info(core, cmd);
        break;
    case APP_CMD_PROGRAM:
    case APP_CMD_WIPE:
        cmd_job(core, cmd);
        break;
    case APP_CMD_CANCEL:
        cmd_cancel(core, cmd);
        break;
    case APP_CMD_HID:
        cmd_hid(core, cmd);
        break;
    case APP_CMD_LOG:
        if (core->env.set_log_level) {
            core->env.set_log_level(core->env.ctx, cmd->level);
        }
        respond(core, cmd, APP_ERR_NONE);
        break;
    case APP_CMD_BOOTLOADER:
        respond(core, cmd, APP_ERR_NONE);
        if (core->job_active) {
            job_failed(core, APP_ERR_CANCELLED, NULL);
        }
        feedback(core, APP_FB_BOOTLOADER);
        if (core->env.enter_bootloader) {
            core->env.enter_bootloader(core->env.ctx);
        }
        break;
    case APP_CMD_DEBUG:
        if (core->env.debug == NULL) {
            respond(core, cmd, APP_ERR_UNKNOWN_CMD);
            break;
        }
        respond(core, cmd, APP_ERR_NONE);       /* first: the action may not return */
        core->env.debug(core->env.ctx, cmd->debug);
        break;
    case APP_CMD_NET:
        if (core->env.net == NULL) {
            respond(core, cmd, APP_ERR_UNKNOWN_CMD);
            break;
        }
        cmd_net(core, cmd);
        break;
    case APP_CMD_OTA:
        if (core->env.ota == NULL) {
            respond(core, cmd, APP_ERR_UNKNOWN_CMD);
            break;
        }
        if (core->job_active) {
            respond(core, cmd, APP_ERR_BUSY);   /* a tag may be half written; not now */
            break;
        }
        {
            const char *detail = NULL;
            respond_detail(core, cmd, core->env.ota(core->env.ctx, cmd, &detail), detail);
        }
        break;
    }
}

void app_core_tick(app_core_t *core)
{
    if (!core->job_active) {
        return;
    }
    /* Signed difference, so the comparison survives the millisecond counter wrapping. */
    if ((int32_t)(core->env.now_ms(core->env.ctx) - core->job_deadline) >= 0) {
        job_failed(core, APP_ERR_TIMEOUT, NULL);
    }
}

bool app_core_wants_tag(const app_core_t *core)
{
    return core->job_active;
}

/* Everything this firmware writes is an NTAG21x: a 7-byte UID and SAK 00. */
static bool looks_like_type2(const app_tag_t *tag)
{
    return tag->uid_len == 7 && tag->sak == 0x00;
}

static bool printable(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (; *s; s++) {
        if (*s < 0x20 || *s > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool lookup(app_core_t *core, const app_tag_t *tag, nfc_xcvr_t *x)
{
    app_evt_t evt = { .type = APP_EVT_TAG };
    set_uid(&evt, tag);
    evt.tag_type = "unknown";
    core->ndef.has_text = false;

    if (looks_like_type2(tag)) {
        ntag_err_t err = ntag_identify(&core->tag, x);
        if (err == NTAG_ERR_REMOVED || err == NTAG_ERR_IO) {
            return false;               /* gone before it could be read: not a tap */
        }
        if (err == NTAG_OK) {
            ntag_state_t st;
            const uint8_t *msg = NULL;
            err = ntag_inspect(&core->tag, &st, &msg);
            if (err != NTAG_OK) {
                return false;
            }
            evt.tag_type = ntag_type_name(core->tag.type);
            evt.has_protected = true;
            evt.is_protected = st.is_protected;
            if (st.has_ndef && msg && ndef_parse_message(msg, st.ndef_len, &core->ndef)) {
                evt.text = core->ndef.has_text ? core->ndef.text : NULL;
                evt.uri = core->ndef.has_uri ? core->ndef.uri : NULL;
            } else {
                core->ndef.has_text = false;
            }
        }
    }

    broadcast(core, &evt);
    feedback(core, evt.text ? APP_FB_TAG_OK : APP_FB_TAG_UNKNOWN);
    if (evt.text && app_core_hid_enabled(core) && core->env.hid_type && printable(evt.text)) {
        core->env.hid_type(core->env.ctx, evt.text);
    }
    return true;
}

typedef struct {
    app_core_t *core;
    const app_tag_t *tag;
} writing_ctx_t;

static void announce_writing(void *ctx)
{
    const writing_ctx_t *w = ctx;
    app_evt_t evt = {
        .type = APP_EVT_WRITING,
        .has_id = true,
        .id = w->core->job.id,
    };
    set_uid(&evt, w->tag);
    broadcast(w->core, &evt);
    feedback(w->core, APP_FB_JOB_WRITING);
}

static void run_job(app_core_t *core, const app_tag_t *tag, nfc_xcvr_t *x)
{
    const app_cmd_t *job = &core->job;
    app_evt_t evt = { 0 };
    set_uid(&evt, tag);

    if (!looks_like_type2(tag)) {
        job_failed(core, APP_ERR_WRONG_TAG_TYPE, &evt);
        return;
    }
    ntag_err_t err = ntag_identify(&core->tag, x);
    if (err != NTAG_OK) {
        job_failed(core, err_of_ntag(err), &evt);
        return;
    }

    writing_ctx_t w = { .core = core, .tag = tag };
    ntag_state_t st = { 0 };
    if (job->type == APP_CMD_WIPE) {
        err = ntag_wipe(&core->tag, job->has_pwd ? job->pwd : NULL, announce_writing, &w);
    } else {
        ntag_program_t p = {
            .ndef = job->ndef,
            .ndef_len = job->ndef_len,
            .overwrite = job->overwrite,
            .have_pwd = job->has_pwd,
            .have_old_pwd = job->has_old_pwd,
            .auth0 = core->auth0,
            .on_writing = announce_writing,
            .ctx = &w,
        };
        memcpy(p.pwd, job->pwd, sizeof(p.pwd));
        memcpy(p.pack, job->pack, sizeof(p.pack));
        memcpy(p.old_pwd, job->old_pwd, sizeof(p.old_pwd));

        const uint8_t *existing = NULL;
        err = ntag_program(&core->tag, &p, &st, &existing);
        if (err == NTAG_ERR_NOT_BLANK && existing
                && ndef_parse_message(existing, st.ndef_len, &core->ndef)) {
            /* Say what is already there, so the page can ask before overwriting. */
            evt.text = core->ndef.has_text ? core->ndef.text : NULL;
            evt.uri = core->ndef.has_uri ? core->ndef.uri : NULL;
        }
    }

    if (err != NTAG_OK) {
        job_failed(core, err_of_ntag(err), &evt);
        return;
    }

    evt.type = APP_EVT_DONE;
    evt.has_id = true;
    evt.id = job->id;
    evt.tag_type = ntag_type_name(core->tag.type);
    evt.has_protected = true;
    evt.is_protected = st.is_protected;
    core->job_active = false;
    broadcast(core, &evt);
    feedback(core, APP_FB_JOB_DONE);
    show_resting_state(core);
}

bool app_core_tag_arrived(app_core_t *core, const app_tag_t *tag, nfc_xcvr_t *x)
{
    bool handled = true;
    if (core->job_active) {
        run_job(core, tag, x);          /* always ends the job, and says how */
    } else {
        handled = lookup(core, tag, x);
    }
    if (handled) {
        core->tag_here = true;
        core->here = *tag;
    }
    return handled;
}

void app_core_tag_conflict(app_core_t *core)
{
    if (core->job_active) {
        job_failed(core, APP_ERR_MULTIPLE_TAGS, NULL);
        return;
    }
    app_evt_t evt = { .type = APP_EVT_TAG, .error = APP_ERR_MULTIPLE_TAGS };
    broadcast(core, &evt);
    feedback(core, APP_FB_TAG_UNKNOWN);
}

void app_core_tag_removed(app_core_t *core, const app_tag_t *tag)
{
    core->tag_here = false;
    app_evt_t evt = { .type = APP_EVT_TAG_REMOVED };
    set_uid(&evt, tag);
    broadcast(core, &evt);
}

void app_core_nfc_state(app_core_t *core, bool ok)
{
    if (core->nfc_ok == ok) {
        return;
    }
    core->nfc_ok = ok;
    if (!ok) {
        core->tag_here = false;
    }
    if (!ok && core->job_active) {
        job_failed(core, APP_ERR_NFC_ERROR, NULL);     /* also shows the resting state */
        return;
    }
    show_resting_state(core);
}

void app_core_link(app_core_t *core, uint8_t origin, bool up)
{
    if (!up) {
        if (core->hid_override_set && core->hid_override_origin == origin) {
            core->hid_override_set = false;
        }
        if (core->job_active && core->job.origin == origin) {
            /* Nobody is left to see the outcome; the next tag must not be written unobserved. */
            job_failed(core, APP_ERR_CANCELLED, NULL);
        }
        return;
    }
    app_sysinfo_t sys = { 0 };
    core->env.sysinfo(core->env.ctx, &sys);
    app_evt_t evt = { .type = APP_EVT_HELLO, .origin = origin, .sys = &sys };
    emit(core, &evt);
}
