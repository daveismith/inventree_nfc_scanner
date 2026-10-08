#include "ntag21x.h"

#include <string.h>

#include "ndef.h"

#define CMD_GET_VERSION 0x60
#define CMD_READ        0x30
#define CMD_WRITE       0xA2
#define CMD_PWD_AUTH    0x1B

#define PAGE_LOCK   2           /* static lock bits in bytes 2 and 3 */
#define PAGE_CC     3
#define PAGE_DATA   4
#define PAGE_SIZE   4
#define READ_SIZE   16          /* READ returns four pages */

#define ACCESS_PROT     0x80
#define ACCESS_CFGLCK   0x40
#define ACCESS_AUTHLIM  0x07

/* How one exchange with the tag ended, before it is given a meaning by the caller. */
typedef enum {
    X_OK,
    X_REFUSED,      /* the tag is still there, and would not do it */
    X_REMOVED,
    X_IO,
} xres_t;

static const uint8_t s_empty_page[PAGE_SIZE] = { 0x03, 0x00, 0xFE, 0x00 };

const char *ntag_type_name(ntag_type_t type)
{
    switch (type) {
    case NTAG_213: return "ntag213";
    case NTAG_215: return "ntag215";
    case NTAG_216: return "ntag216";
    default:       return "unknown";
    }
}

static ntag_err_t err_of(xres_t r, ntag_err_t refused)
{
    switch (r) {
    case X_OK:      return NTAG_OK;
    case X_REMOVED: return NTAG_ERR_REMOVED;
    case X_IO:      return NTAG_ERR_IO;
    default:        return refused;
    }
}

/* After any failed exchange the tag is in IDLE: select it again, and authenticate again if it
 * was authenticated, since that state does not survive. */
static xres_t recover(ntag_t *t)
{
    nfc_err_t e = t->x->reselect(t->x);
    if (e == NFC_ERR_IO) {
        return X_IO;
    }
    if (e != NFC_OK) {
        return X_REMOVED;
    }
    if (!t->authed) {
        return X_OK;
    }

    const uint8_t tx[5] = { CMD_PWD_AUTH, t->key[0], t->key[1], t->key[2], t->key[3] };
    uint8_t rx[2];
    size_t n = 0;
    e = t->x->transceive(t->x, tx, sizeof(tx), rx, sizeof(rx), &n);
    if (e == NFC_OK) {
        return X_OK;
    }
    t->authed = false;
    if (e == NFC_ERR_IO) {
        return X_IO;
    }
    e = t->x->reselect(t->x);
    return e == NFC_OK ? X_REFUSED : (e == NFC_ERR_IO ? X_IO : X_REMOVED);
}

/*
 * One command. A NAK is final. Silence is retried once if the tag turns out to be there still,
 * which is what marginal coupling looks like.
 */
static xres_t xfer(ntag_t *t, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_cap,
                   size_t *rx_len)
{
    size_t n = 0;
    if (rx_len == NULL) {
        rx_len = &n;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        const nfc_err_t e = t->x->transceive(t->x, tx, tx_len, rx, rx_cap, rx_len);
        if (e == NFC_OK) {
            return X_OK;
        }
        if (e == NFC_ERR_IO) {
            return X_IO;
        }
        const xres_t r = recover(t);
        if (r != X_OK) {
            return r;
        }
        if (e == NFC_ERR_NAK) {
            return X_REFUSED;
        }
    }
    return X_REFUSED;
}

static xres_t read4(ntag_t *t, uint8_t page, uint8_t out[READ_SIZE])
{
    const uint8_t tx[2] = { CMD_READ, page };
    size_t n = 0;
    const xres_t r = xfer(t, tx, sizeof(tx), out, READ_SIZE, &n);
    if (r == X_OK && n != READ_SIZE) {
        return X_REFUSED;
    }
    return r;
}

static xres_t write_page(ntag_t *t, uint8_t page, const uint8_t data[PAGE_SIZE])
{
    const uint8_t tx[2 + PAGE_SIZE] = { CMD_WRITE, page, data[0], data[1], data[2], data[3] };
    uint8_t rx[4];
    return xfer(t, tx, sizeof(tx), rx, sizeof(rx), NULL);
}

ntag_err_t ntag_identify(ntag_t *t, nfc_xcvr_t *x)
{
    memset(t, 0, sizeof(*t));
    t->x = x;

    const uint8_t tx[1] = { CMD_GET_VERSION };
    uint8_t rx[8];
    size_t n = 0;
    const xres_t r = xfer(t, tx, sizeof(tx), rx, sizeof(rx), &n);
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_WRONG_TYPE);
    }
    /* vendor NXP, product type NTAG; byte 6 is the storage size */
    if (n != sizeof(rx) || rx[1] != 0x04 || rx[2] != 0x04) {
        return NTAG_ERR_WRONG_TYPE;
    }
    switch (rx[6]) {
    case 0x0F:
        t->type = NTAG_213;
        t->cfg_page = 0x29;
        t->user_bytes = 144;
        t->cc_size = 0x12;
        break;
    case 0x11:
        t->type = NTAG_215;
        t->cfg_page = 0x83;
        t->user_bytes = 504;
        t->cc_size = 0x3E;
        break;
    case 0x13:
        t->type = NTAG_216;
        t->cfg_page = 0xE3;
        t->user_bytes = 888;
        t->cc_size = 0x6D;
        break;
    default:
        return NTAG_ERR_WRONG_TYPE;
    }
    return NTAG_OK;
}

ntag_err_t ntag_auth(ntag_t *t, const uint8_t pwd[4], uint8_t pack[2])
{
    const uint8_t tx[5] = { CMD_PWD_AUTH, pwd[0], pwd[1], pwd[2], pwd[3] };
    uint8_t rx[2];
    size_t n = 0;

    /* Whatever authenticated before no longer matters, and must not be replayed by recover(). */
    t->authed = false;
    const xres_t r = xfer(t, tx, sizeof(tx), rx, sizeof(rx), &n);
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_AUTH_FAILED);
    }
    if (n != sizeof(rx)) {
        return NTAG_ERR_AUTH_FAILED;
    }
    t->authed = true;
    memcpy(t->key, pwd, sizeof(t->key));
    if (pack) {
        memcpy(pack, rx, sizeof(rx));
    }
    return NTAG_OK;
}

static uint16_t default_capacity(const ntag_t *t)
{
    const uint16_t declared = (uint16_t)t->cc_size * 8;
    return declared < t->user_bytes ? declared : t->user_bytes;
}

ntag_err_t ntag_inspect(ntag_t *t, ntag_state_t *st, const uint8_t **msg)
{
    uint8_t buf[READ_SIZE];
    memset(st, 0, sizeof(*st));
    if (msg) {
        *msg = NULL;
    }

    /* Dynamic lock bytes, CFG0 and CFG1 in one read. With PROT=1 and AUTH0 at or below these
     * pages the tag refuses, and nothing more can be learned without the password. */
    xres_t r = read4(t, t->cfg_page - 1, buf);
    if (r == X_REFUSED) {
        st->is_protected = true;
        return NTAG_OK;
    }
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_IO);
    }
    st->readable = true;
    const bool dyn_locked = buf[0] != 0 || buf[1] != 0;
    st->auth0 = buf[4 + 3];
    st->access = buf[8];
    st->is_protected = st->auth0 <= t->cfg_page + 3;
    st->cfg_locked = (st->access & ACCESS_CFGLCK) != 0;

    /* Static lock bits, the capability container and the first two data pages. */
    r = read4(t, PAGE_LOCK, buf);
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_IO);
    }
    const uint8_t *cc = buf + 4;
    const bool cc_blank = cc[0] == 0 && cc[1] == 0 && cc[2] == 0 && cc[3] == 0;
    st->formatted = cc[0] == 0xE1;
    if (st->formatted) {
        const uint16_t declared = (uint16_t)cc[2] * 8;
        st->capacity = declared < t->user_bytes ? declared : t->user_bytes;
    } else {
        st->capacity = default_capacity(t);
    }
    /* Byte 2 bits 0-2 only freeze other lock bits; bit 3 and up lock pages 3 to 15. The CC is
     * one-time programmable, so one that is neither blank nor NDEF can never be made right. */
    st->locked = (buf[2] & 0xF8) != 0 || buf[3] != 0 || dyn_locked
                 || (st->formatted && (cc[3] & 0x0F) != 0)
                 || (!st->formatted && !cc_blank);
    if (!st->formatted) {
        return NTAG_OK;
    }

    memcpy(t->area, buf + 8, 8);
    size_t have = 8;
    for (;;) {
        size_t off = 0, len = 0, need = 0;
        /* A read can run past the data area into the lock and configuration pages. */
        const size_t usable = have < st->capacity ? have : st->capacity;
        const t2t_find_t f = t2t_find_ndef(t->area, usable, &off, &len, &need);
        if (f == T2T_NDEF_FOUND) {
            st->has_ndef = true;
            st->ndef_len = len;
            if (msg) {
                *msg = t->area + off;
            }
            break;
        }
        if (f != T2T_NDEF_MORE || need > st->capacity) {
            break;                      /* nothing there that can be read as a message */
        }
        while (have < need) {
            r = read4(t, (uint8_t)(PAGE_DATA + have / PAGE_SIZE), t->area + have);
            if (r != X_OK) {
                return err_of(r, NTAG_ERR_IO);
            }
            have += READ_SIZE;
        }
    }
    return NTAG_OK;
}

/* Inspect, authenticating first if the tag will not even be read without it. */
static ntag_err_t inspect_with_key(ntag_t *t, ntag_state_t *st, const uint8_t **msg, const uint8_t *key)
{
    ntag_err_t err = ntag_inspect(t, st, msg);
    if (err != NTAG_OK || st->readable) {
        return err;
    }
    if (key == NULL) {
        return NTAG_ERR_AUTH_REQUIRED;
    }
    err = ntag_auth(t, key, NULL);
    if (err != NTAG_OK) {
        return err;
    }
    err = ntag_inspect(t, st, msg);
    if (err == NTAG_OK && !st->readable) {
        err = NTAG_ERR_AUTH_FAILED;
    }
    return err;
}

/*
 * PWD, PACK, then ACCESS, and AUTH0 last: until AUTH0 is written the tag is still open, so a
 * tag pulled part-way is never left locked behind a half-written password.
 */
static ntag_err_t protect(ntag_t *t, const ntag_state_t *st, const ntag_program_t *p)
{
    uint8_t cfg[READ_SIZE];
    xres_t r = read4(t, t->cfg_page, cfg);
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_IO);
    }

    uint8_t cfg0[PAGE_SIZE], cfg1[PAGE_SIZE];
    memcpy(cfg0, cfg, PAGE_SIZE);
    memcpy(cfg1, cfg + PAGE_SIZE, PAGE_SIZE);
    cfg0[3] = p->auth0;
    cfg1[0] &= (uint8_t)~(ACCESS_PROT | ACCESS_AUTHLIM);    /* write protection only; no attempt limit */
    const bool cfg0_changes = memcmp(cfg0, cfg, PAGE_SIZE) != 0;
    const bool cfg1_changes = memcmp(cfg1, cfg + PAGE_SIZE, PAGE_SIZE) != 0;
    if ((cfg0_changes || cfg1_changes) && st->cfg_locked) {
        return NTAG_ERR_LOCKED;
    }

    r = write_page(t, t->cfg_page + 2, p->pwd);
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_WRITE);
    }
    if (t->authed) {
        memcpy(t->key, p->pwd, sizeof(t->key));     /* the old key no longer opens it */
    }
    const uint8_t pack_page[PAGE_SIZE] = { p->pack[0], p->pack[1], 0, 0 };
    r = write_page(t, t->cfg_page + 3, pack_page);
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_WRITE);
    }
    if (cfg1_changes) {
        r = write_page(t, t->cfg_page + 1, cfg1);
        if (r != X_OK) {
            return err_of(r, NTAG_ERR_WRITE);
        }
    }
    if (cfg0_changes) {
        r = write_page(t, t->cfg_page, cfg0);
        if (r != X_OK) {
            return err_of(r, NTAG_ERR_WRITE);
        }
    }

    /* Prove it from cold: a fresh selection, the new password, the expected acknowledge. */
    t->authed = false;
    const nfc_err_t e = t->x->reselect(t->x);
    if (e != NFC_OK) {
        return e == NFC_ERR_IO ? NTAG_ERR_IO : NTAG_ERR_REMOVED;
    }
    uint8_t pack[2];
    ntag_err_t err = ntag_auth(t, p->pwd, pack);
    if (err == NTAG_ERR_AUTH_FAILED) {
        return NTAG_ERR_VERIFY;
    }
    if (err != NTAG_OK) {
        return err;
    }
    if (memcmp(pack, p->pack, sizeof(pack)) != 0) {
        return NTAG_ERR_VERIFY;
    }
    r = read4(t, t->cfg_page, cfg);
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_VERIFY);
    }
    if (cfg[3] != p->auth0 || (cfg[PAGE_SIZE] & ACCESS_PROT) != 0) {
        return NTAG_ERR_VERIFY;
    }
    return NTAG_OK;
}

ntag_err_t ntag_program(ntag_t *t, const ntag_program_t *p, ntag_state_t *st, const uint8_t **existing)
{
    ntag_state_t local;
    if (st == NULL) {
        st = &local;
    }
    const uint8_t *key = p->have_old_pwd ? p->old_pwd : (p->have_pwd ? p->pwd : NULL);
    const uint8_t *msg = NULL;

    ntag_err_t err = inspect_with_key(t, st, &msg, key);
    if (existing) {
        *existing = msg;
    }
    if (err != NTAG_OK) {
        return err;
    }
    if (st->locked) {
        return NTAG_ERR_LOCKED;
    }
    if (p->ndef_len + t2t_tlv_overhead(p->ndef_len) > st->capacity) {
        return NTAG_ERR_TOO_LARGE;
    }
    /* A tag a phone has erased holds an Empty record, not nothing; that is still blank. */
    if (st->has_ndef && !ndef_message_is_empty(msg, st->ndef_len) && !p->overwrite) {
        return NTAG_ERR_NOT_BLANK;
    }
    if (st->is_protected && !t->authed) {
        if (key == NULL) {
            return NTAG_ERR_AUTH_REQUIRED;
        }
        err = ntag_auth(t, key, NULL);
        if (err != NTAG_OK) {
            return err;
        }
    }

    /* From here on the tag is written, and `msg` (which points into the scratch area) is dead. */
    if (existing) {
        *existing = NULL;
    }
    if (p->on_writing) {
        p->on_writing(p->ctx);
    }

    xres_t r;
    if (!st->formatted) {
        const uint8_t cc[PAGE_SIZE] = { 0xE1, 0x10, t->cc_size, 0x00 };
        r = write_page(t, PAGE_CC, cc);
        if (r != X_OK) {
            return err_of(r, NTAG_ERR_WRITE);
        }
    }

    size_t n = t2t_wrap_ndef(p->ndef, p->ndef_len, t->area, st->capacity);
    if (n == 0) {
        return NTAG_ERR_TOO_LARGE;
    }
    while (n % PAGE_SIZE) {
        t->area[n++] = 0;
    }

    /* The tag says "empty" until the last write puts the real length in the first page, so
     * being pulled at any point leaves a valid tag. The T and L bytes all sit in that page. */
    r = write_page(t, PAGE_DATA, s_empty_page);
    for (size_t i = PAGE_SIZE; r == X_OK && i < n; i += PAGE_SIZE) {
        r = write_page(t, (uint8_t)(PAGE_DATA + i / PAGE_SIZE), t->area + i);
    }
    if (r == X_OK) {
        r = write_page(t, PAGE_DATA, t->area);
    }
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_WRITE);
    }

    for (size_t i = 0; i < n; i += READ_SIZE) {
        uint8_t back[READ_SIZE];
        r = read4(t, (uint8_t)(PAGE_DATA + i / PAGE_SIZE), back);
        if (r != X_OK) {
            return err_of(r, NTAG_ERR_VERIFY);
        }
        const size_t cmp = n - i < READ_SIZE ? n - i : READ_SIZE;
        if (memcmp(back, t->area + i, cmp) != 0) {
            return NTAG_ERR_VERIFY;
        }
    }

    if (p->have_pwd) {
        err = protect(t, st, p);
        if (err != NTAG_OK) {
            return err;
        }
        st->is_protected = true;
    }
    return NTAG_OK;
}

ntag_err_t ntag_wipe(ntag_t *t, const uint8_t *pwd, void (*on_writing)(void *ctx), void *ctx)
{
    ntag_state_t st;
    ntag_err_t err = inspect_with_key(t, &st, NULL, pwd);
    if (err != NTAG_OK) {
        return err;
    }
    if (st.locked) {
        return NTAG_ERR_LOCKED;
    }
    if (st.is_protected && !t->authed) {
        if (pwd == NULL) {
            return NTAG_ERR_AUTH_REQUIRED;
        }
        err = ntag_auth(t, pwd, NULL);
        if (err != NTAG_OK) {
            return err;
        }
    }

    if (st.auth0 != NTAG_AUTH0_OFF && st.cfg_locked) {
        return NTAG_ERR_LOCKED;         /* the config pages would have to change and cannot: write nothing */
    }

    if (on_writing) {
        on_writing(ctx);
    }

    xres_t r;
    if (!st.formatted) {
        const uint8_t cc[PAGE_SIZE] = { 0xE1, 0x10, t->cc_size, 0x00 };
        r = write_page(t, PAGE_CC, cc);
        if (r != X_OK) {
            return err_of(r, NTAG_ERR_WRITE);
        }
    }
    r = write_page(t, PAGE_DATA, s_empty_page);
    if (r != X_OK) {
        return err_of(r, NTAG_ERR_WRITE);
    }

    if (st.auth0 != NTAG_AUTH0_OFF) {
        uint8_t cfg[READ_SIZE];
        r = read4(t, t->cfg_page, cfg);
        if (r != X_OK) {
            return err_of(r, NTAG_ERR_IO);
        }
        /* Open the tag first; the password pages are freely writable once it is. */
        cfg[3] = NTAG_AUTH0_OFF;
        r = write_page(t, t->cfg_page, cfg);
        if (r != X_OK) {
            return err_of(r, NTAG_ERR_WRITE);
        }
        static const uint8_t pwd_default[PAGE_SIZE] = { 0xFF, 0xFF, 0xFF, 0xFF };
        static const uint8_t pack_default[PAGE_SIZE] = { 0, 0, 0, 0 };
        r = write_page(t, t->cfg_page + 2, pwd_default);
        if (r == X_OK) {
            r = write_page(t, t->cfg_page + 3, pack_default);
        }
        if (r != X_OK) {
            return err_of(r, NTAG_ERR_WRITE);
        }
        t->authed = false;
    }
    return NTAG_OK;
}
