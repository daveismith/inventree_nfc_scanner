/*
 * NTAG213/215/216 operations: identify, read the NDEF message, program one tear-safely with
 * read-back verification, and password-protect with PWD/PACK/AUTH0.
 *
 * Page addresses are from the NXP NTAG213/215/216 data sheet (section 8.5, tables 5-11):
 * configuration pages at 29h-2Ch, 83h-86h and E3h-E6h. The permanent lock bits are read, to
 * refuse a tag that has them set, and never written.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nfc_xcvr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NTAG_AREA_MAX   888         /* NTAG216 user memory, the largest */
#define NTAG_AUTH0_OFF  0xFF        /* factory value: no page needs the password */

/* First page that needs the password once a tag is protected. 03h covers the capability
 * container as well as the user memory, dynamic lock bytes and configuration pages. */
#define NTAG_AUTH0_DEFAULT 0x03

typedef enum {
    NTAG_UNKNOWN = 0,
    NTAG_213,
    NTAG_215,
    NTAG_216,
} ntag_type_t;

typedef enum {
    NTAG_OK = 0,
    NTAG_ERR_REMOVED,           /* the tag left the field */
    NTAG_ERR_IO,                /* the reader failed */
    NTAG_ERR_WRONG_TYPE,        /* not an NTAG21x */
    NTAG_ERR_NOT_BLANK,         /* holds a message and overwrite was not asked for */
    NTAG_ERR_AUTH_REQUIRED,     /* protected, and no password was given */
    NTAG_ERR_AUTH_FAILED,       /* the tag rejected the password */
    NTAG_ERR_LOCKED,            /* lock bits or the capability container forbid writing */
    NTAG_ERR_TOO_LARGE,         /* the message does not fit */
    NTAG_ERR_WRITE,             /* the tag refused a write */
    NTAG_ERR_VERIFY,            /* read-back did not match */
} ntag_err_t;

typedef struct {
    nfc_xcvr_t *x;
    ntag_type_t type;
    uint8_t cfg_page;           /* CFG0; CFG1, PWD and PACK follow */
    uint16_t user_bytes;        /* from page 4 */
    uint8_t cc_size;            /* capability container byte 2 as delivered */
    bool authed;
    uint8_t key[4];             /* what authenticated, to repeat after a re-select */
    uint8_t area[NTAG_AREA_MAX + 4];    /* scratch: the data area being read or written */
} ntag_t;

typedef struct {
    bool formatted;             /* capability container carries the NDEF magic */
    bool readable;              /* false: PROT=1 and not authenticated, so the rest is unknown */
    bool is_protected;          /* AUTH0 lies within the tag's memory */
    bool locked;                /* cannot be written, permanently */
    bool cfg_locked;            /* CFGLCK: AUTH0 and ACCESS can no longer change */
    uint8_t auth0;
    uint8_t access;
    uint16_t capacity;          /* bytes of data area the tag declares */
    bool has_ndef;              /* an NDEF Message TLV was found */
    size_t ndef_len;            /* 0 for a formatted, empty tag */
} ntag_state_t;

typedef struct {
    const uint8_t *ndef;
    size_t ndef_len;
    bool overwrite;
    bool have_pwd;              /* protect with pwd/pack afterwards; also the key if protected */
    uint8_t pwd[4];
    uint8_t pack[2];
    bool have_old_pwd;          /* the key to use instead, when rotating the password */
    uint8_t old_pwd[4];
    uint8_t auth0;              /* NTAG_AUTH0_DEFAULT unless there is a reason */
    void (*on_writing)(void *ctx);  /* called once, just before the first write; may be NULL */
    void *ctx;
} ntag_program_t;

const char *ntag_type_name(ntag_type_t type);

/* GET_VERSION. NTAG_ERR_WRONG_TYPE for anything that is not an NTAG213/215/216. */
ntag_err_t ntag_identify(ntag_t *t, nfc_xcvr_t *x);

/* PWD_AUTH. `pack` may be NULL. */
ntag_err_t ntag_auth(ntag_t *t, const uint8_t pwd[4], uint8_t pack[2]);

/*
 * Read the tag's state and its NDEF message, which is left at `*msg` (pointing into the tag
 * object's scratch area, valid until the next call) with its length in st->ndef_len.
 */
ntag_err_t ntag_inspect(ntag_t *t, ntag_state_t *st, const uint8_t **msg);

/*
 * Write an NDEF message. On NTAG_ERR_NOT_BLANK the existing message is at `*existing` with
 * its length in st->ndef_len. `st` is the state before writing, except is_protected, which is
 * the state afterwards. Either may be NULL.
 *
 * A tag that leaves the field part-way holds an empty, valid message: the length is written
 * last, in a single page.
 */
ntag_err_t ntag_program(ntag_t *t, const ntag_program_t *p, ntag_state_t *st, const uint8_t **existing);

/*
 * Empty the message and remove password protection. `pwd` may be NULL for an open tag.
 * `on_writing`, which may be NULL, is called once, just before the first write.
 */
ntag_err_t ntag_wipe(ntag_t *t, const uint8_t *pwd, void (*on_writing)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
