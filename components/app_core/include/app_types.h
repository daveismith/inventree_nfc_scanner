/*
 * The commands the device accepts and the events it reports, as plain structs.
 *
 * `proto` turns lines of JSON into app_cmd_t and app_evt_t into lines; `app_core` consumes the
 * one and produces the other. Neither knows what carries the lines, which is what lets a
 * network transport sit beside USB later.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_PROTO_VERSION       1
#define APP_NDEF_MAX            888         /* the largest NTAG21x data area */
#define APP_UID_MAX             10

#define APP_JOB_TIMEOUT_DEFAULT_MS  60000
#define APP_JOB_TIMEOUT_MIN_MS      1000
#define APP_JOB_TIMEOUT_MAX_MS      600000

typedef enum {
    APP_ERR_NONE = 0,
    APP_ERR_BAD_JSON,
    APP_ERR_LINE_TOO_LONG,
    APP_ERR_UNKNOWN_CMD,
    APP_ERR_BAD_ARG,
    APP_ERR_BUSY,
    APP_ERR_NO_JOB,
    APP_ERR_TIMEOUT,
    APP_ERR_CANCELLED,
    APP_ERR_WRONG_TAG_TYPE,
    APP_ERR_MULTIPLE_TAGS,
    APP_ERR_NOT_BLANK,
    APP_ERR_AUTH_REQUIRED,
    APP_ERR_AUTH_FAILED,
    APP_ERR_LOCKED,
    APP_ERR_TOO_LARGE,
    APP_ERR_TAG_REMOVED,
    APP_ERR_WRITE_FAILED,
    APP_ERR_VERIFY_FAILED,
    APP_ERR_NFC_ERROR,
} app_err_t;

typedef enum {
    APP_CMD_INFO,
    APP_CMD_PROGRAM,
    APP_CMD_WIPE,
    APP_CMD_CANCEL,
    APP_CMD_HID,
    APP_CMD_LOG,
    APP_CMD_BOOTLOADER,
    APP_CMD_DEBUG,              /* development builds only: provoke a failure on purpose */
} app_cmd_type_t;

typedef enum {
    APP_DEBUG_CRASH,            /* panic */
    APP_DEBUG_HANG,             /* stop yielding, for the watchdog to find */
    APP_DEBUG_NOUSB,            /* restart, and stay off the bus on the next boot */
} app_debug_action_t;

typedef enum {
    APP_LOG_OFF,
    APP_LOG_ERROR,
    APP_LOG_WARN,
    APP_LOG_INFO,
    APP_LOG_DEBUG,
} app_log_level_t;

typedef struct {
    app_cmd_type_t type;
    bool has_id;
    int32_t id;

    /* program, wipe */
    uint16_t ndef_len;
    uint8_t ndef[APP_NDEF_MAX];
    bool overwrite;
    bool has_pwd;
    uint8_t pwd[4];
    uint8_t pack[2];
    bool has_old_pwd;
    uint8_t old_pwd[4];
    uint32_t timeout_ms;

    /* hid */
    bool enabled;
    bool persist;

    /* log */
    app_log_level_t level;

    /* debug */
    app_debug_action_t debug;
} app_cmd_t;

/* What `info` reports that the state machine does not itself know. */
typedef struct {
    const char *fw;
    const char *idf;
    bool pn532_ok;
    uint8_t pn532_ic;
    uint8_t pn532_ver;
    uint8_t pn532_rev;
    bool buzzer;
    const char *reset;          /* why the chip last reset */
    const char *crash;          /* one line about the last crash, or NULL */
    uint32_t uptime_ms;
} app_sysinfo_t;

typedef enum {
    APP_EVT_RSP,                /* the one immediate answer every command gets */
    APP_EVT_HELLO,
    APP_EVT_WAITING,
    APP_EVT_WRITING,
    APP_EVT_DONE,
    APP_EVT_FAILED,
    APP_EVT_TAG,
    APP_EVT_TAG_REMOVED,
    APP_EVT_ERROR,              /* a line that could not be read as a command at all */
    APP_EVT_LOG,
} app_evt_type_t;

/* One event. Which fields mean anything depends on `type`; unused pointers are NULL. */
typedef struct {
    app_evt_type_t type;

    const char *cmd;            /* RSP: the command being answered */
    bool ok;
    app_err_t error;
    const char *detail;

    bool has_id;
    int32_t id;
    uint32_t timeout_ms;

    uint8_t uid_len;
    uint8_t uid[APP_UID_MAX];
    const char *tag_type;
    const char *text;
    const char *uri;
    bool has_protected;
    bool is_protected;

    /* RSP to info */
    const app_sysinfo_t *sys;
    const char *state;
    bool has_job;
    int32_t job_id;

    /* RSP to info and hid */
    bool has_hid;
    bool hid;

    /* LOG */
    char lvl;
    const char *log_tag;
    const char *msg;
} app_evt_t;

const char *app_err_name(app_err_t err);
const char *app_cmd_name(app_cmd_type_t type);

#ifdef __cplusplus
}
#endif
