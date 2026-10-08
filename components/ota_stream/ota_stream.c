#include "ota_stream.h"

#include <stdio.h>
#include <string.h>

void ota_stream_init(ota_stream_t *s, const ota_stream_backend_t *backend)
{
    memset(s, 0, sizeof(*s));
    s->backend = *backend;
}

static void end(ota_stream_t *s, bool abort)
{
    if (s->active && abort && s->backend.abort) {
        s->backend.abort(s->backend.ctx);
    }
    s->active = false;
}

static app_err_t fail(ota_stream_t *s, app_err_t err, const char *detail, const char **out)
{
    end(s, true);
    *out = detail;
    return err;
}

app_err_t ota_stream_command(ota_stream_t *s, const app_cmd_t *cmd, uint32_t now_ms, const char **detail)
{
    *detail = NULL;
    if (cmd->type == APP_CMD_OTA_BEGIN) {
        if (s->active && now_ms - s->last_ms < OTA_STREAM_IDLE_MS && cmd->origin != s->origin) {
            *detail = "another link is sending an update";
            return APP_ERR_BUSY;
        }
        end(s, true);               /* starting again: whatever came before is abandoned */
        const char *why = NULL;
        if (!s->backend.begin(s->backend.ctx, cmd->ota_size, &why)) {
            *detail = why ? why : "the update cannot be started";
            return APP_ERR_BAD_ARG;
        }
        s->active = true;
        s->id = cmd->id;
        s->origin = cmd->origin;
        s->size = cmd->ota_size;
        s->at = 0;
        memcpy(s->sha256, cmd->sha256, sizeof(s->sha256));
        s->last_ms = now_ms;
        return APP_ERR_NONE;
    }

    if (!s->active || cmd->id != s->id || cmd->origin != s->origin) {
        *detail = "no update with that id is being sent; start with ota_begin";
        return APP_ERR_NO_JOB;
    }
    s->last_ms = now_ms;

    if (cmd->type == APP_CMD_OTA_DATA) {
        if (cmd->ota_at != s->at) {
            snprintf(s->detail, sizeof(s->detail), "at: expected %lu", (unsigned long)s->at);
            return fail(s, APP_ERR_BAD_ARG, s->detail, detail);
        }
        if ((uint32_t)cmd->ndef_len > s->size - s->at) {
            return fail(s, APP_ERR_BAD_ARG, "data: more than the size given", detail);
        }
        const char *why = NULL;
        if (!s->backend.write(s->backend.ctx, cmd->ndef, cmd->ndef_len, &why)) {
            return fail(s, APP_ERR_WRITE_FAILED, why ? why : "could not write the image", detail);
        }
        s->at += cmd->ndef_len;
        return APP_ERR_NONE;
    }

    if (cmd->type == APP_CMD_OTA_END) {
        if (s->at != s->size) {
            snprintf(s->detail, sizeof(s->detail), "only %lu of %lu bytes sent", (unsigned long)s->at,
                     (unsigned long)s->size);
            return fail(s, APP_ERR_BAD_ARG, s->detail, detail);
        }
        const char *why = NULL;
        if (!s->backend.finish(s->backend.ctx, s->sha256, &why)) {
            return fail(s, APP_ERR_VERIFY_FAILED, why ? why : "the image is not valid", detail);
        }
        end(s, false);
        return APP_ERR_NONE;
    }
    *detail = "not an update command";
    return APP_ERR_UNKNOWN_CMD;
}

bool ota_stream_link_down(ota_stream_t *s, uint8_t origin)
{
    if (!s->active || s->origin != origin) {
        return false;
    }
    end(s, true);
    return true;
}

bool ota_stream_expire(ota_stream_t *s, uint32_t now_ms)
{
    if (!s->active || now_ms - s->last_ms < OTA_STREAM_IDLE_MS) {
        return false;
    }
    end(s, true);
    return true;
}
