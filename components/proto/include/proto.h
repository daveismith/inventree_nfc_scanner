/*
 * The serial protocol, version 1: newline-delimited JSON, one object a line.
 *
 * Host to device: {"cmd":"program","id":7,"ndef":"D1010B...","pwd":"A1B2C3D4"} and friends.
 * Device to host: one {"rsp":...} per command, then {"evt":...} lines as things happen.
 *
 * This is only the mapping between lines and the structs in app_types.h. It does no I/O, so a
 * line can come from USB CDC, a pty in the host simulator, or a socket later.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROTO_LINE_MAX 2048     /* longest line accepted, terminator included */

typedef struct {
    app_err_t error;
    const char *cmd;            /* the command's name when it was recognised, else NULL */
    bool has_id;
    int32_t id;
    char detail[64];            /* which field, and what was wrong with it */
    char name[24];              /* an unknown command's name, which `cmd` then points at */
} proto_err_t;

/*
 * Parse one line (no terminator needed) into a command. On false, `err` says why; turn it
 * into the line to send back with proto_err_event().
 */
bool proto_parse(const char *line, size_t len, app_cmd_t *cmd, proto_err_t *err);

/* A parse failure as an event: a failed `rsp` when the command was recognised, else `error`. */
void proto_err_event(const proto_err_t *err, app_evt_t *evt);

/*
 * Render an event as one line, '\n' included and NUL terminated. Returns the line's length,
 * or 0 when it does not fit in `cap` or memory ran out.
 */
size_t proto_format(const app_evt_t *evt, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
