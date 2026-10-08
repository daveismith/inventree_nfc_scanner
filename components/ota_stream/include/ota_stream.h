/*
 * A firmware image sent over the serial link: the session, kept apart from where the bytes
 * go, so that the firmware (flash, with its own hash) and the simulator share it.
 *
 *   ota_begin {id, size, sha256}   a session starts; the backend gets ready for `size` bytes
 *   ota_data  {id, at, data}       the next piece, in order: `at` must be where the last ended
 *   ota_end   {id}                 all `size` bytes are in; the backend checks the digest and
 *                                  makes the image the one to boot
 *
 * A session ends when it is finished, when anything goes wrong (the backend is told to abort,
 * and the next ota_begin starts again from nothing), when the link it came over goes down,
 * or after OTA_STREAM_IDLE_MS with nothing from it. Only one at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_types.h"

#define OTA_STREAM_IDLE_MS  30000

typedef struct {
    void *ctx;
    /* Get ready for an image of `size` bytes; false (with a detail) if it cannot be taken. */
    bool (*begin)(void *ctx, uint32_t size, const char **detail);
    bool (*write)(void *ctx, const uint8_t *data, size_t len, const char **detail);
    /* Everything is in: the digest of what was written must be `sha256`, and the image must
     * be valid; then it becomes the one to boot. */
    bool (*finish)(void *ctx, const uint8_t sha256[32], const char **detail);
    void (*abort)(void *ctx);
} ota_stream_backend_t;

typedef struct {
    ota_stream_backend_t backend;
    bool active;
    int32_t id;
    uint8_t origin;
    uint32_t size;
    uint32_t at;
    uint8_t sha256[32];
    uint32_t last_ms;
    char detail[64];
} ota_stream_t;

void ota_stream_init(ota_stream_t *s, const ota_stream_backend_t *backend);

/* ota_begin, ota_data or ota_end. APP_ERR_NONE for ota_end means the image is ready to boot:
 * the caller restarts into it. Anything other than NONE has ended the session (except a busy
 * ota_begin, which leaves the other session be). */
app_err_t ota_stream_command(ota_stream_t *s, const app_cmd_t *cmd, uint32_t now_ms, const char **detail);

/* The link a session came over went down: it ends. True if one did. */
bool ota_stream_link_down(ota_stream_t *s, uint8_t origin);

/* Call now and then: a session idle too long ends. True if one did. */
bool ota_stream_expire(ota_stream_t *s, uint32_t now_ms);

static inline bool ota_stream_active(const ota_stream_t *s)
{
    return s->active;
}

/* How far along: bytes received of the size, for progress. */
static inline uint32_t ota_stream_received(const ota_stream_t *s)
{
    return s->at;
}
