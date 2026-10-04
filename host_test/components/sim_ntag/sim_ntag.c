#include "sim_ntag.h"

#include <string.h>

#include "ndef.h"

#define PAGE_DATA 4

static uint8_t auth0(const sim_ntag_t *s)
{
    return s->pages[s->cfg_page][3];
}

static uint8_t access(const sim_ntag_t *s)
{
    return s->pages[s->cfg_page + 1][0];
}

/* A page needs the password when it is at or above AUTH0 and nobody has authenticated. */
static bool needs_auth(const sim_ntag_t *s, uint8_t page)
{
    return !s->authed && page >= auth0(s);
}

static nfc_err_t nak(sim_ntag_t *s)
{
    s->selected = false;
    s->authed = false;
    return NFC_ERR_NAK;
}

static nfc_err_t do_read(sim_ntag_t *s, uint8_t page, uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    const bool prot_read = (access(s) & 0x80) != 0;
    if (page > s->last_page || rx_cap < 16 || (prot_read && needs_auth(s, page))) {
        return nak(s);
    }
    /* Four pages, rolling over to page 0 after the last readable one. */
    const uint8_t limit = (prot_read && !s->authed && auth0(s) <= s->last_page) ? auth0(s) : s->last_page + 1;
    uint8_t p = page;
    for (int i = 0; i < 4; i++) {
        if (p >= limit) {
            p = 0;
        }
        if (p == s->cfg_page + 2 || p == s->cfg_page + 3) {
            memset(rx + 4 * i, 0, 4);       /* PWD and PACK never read back */
        } else {
            memcpy(rx + 4 * i, s->pages[p], 4);
        }
        p++;
    }
    *rx_len = 16;
    return NFC_OK;
}

static bool page_locked(const sim_ntag_t *s, uint8_t page)
{
    const uint8_t lock0 = s->pages[2][2];
    const uint8_t lock1 = s->pages[2][3];
    if (page >= 3 && page <= 7) {
        return (lock0 >> page) & 1;
    }
    if (page >= 8 && page <= 15) {
        return (lock1 >> (page - 8)) & 1;
    }
    if (page >= 16 && page < s->cfg_page - 1) {
        /* Dynamic lock bits each cover a run of pages; any set bit locks them all here, which
         * is coarser than the tag and enough for firmware that never sets one. */
        return s->pages[s->cfg_page - 1][0] != 0 || s->pages[s->cfg_page - 1][1] != 0;
    }
    return false;
}

static nfc_err_t do_write(sim_ntag_t *s, uint8_t page, const uint8_t *data)
{
    if (page < 2 || page > s->last_page || needs_auth(s, page) || page_locked(s, page)) {
        return nak(s);
    }
    const bool cfglck = (access(s) & 0x40) != 0;
    if (cfglck && (page == s->cfg_page || page == s->cfg_page + 1)) {
        return nak(s);
    }

    if (s->tear_after_writes == 0) {
        s->present = false;             /* pulled away: this write never lands */
        s->tear_after_writes = -1;
        return NFC_ERR_TIMEOUT;
    }
    if (s->tear_after_writes > 0) {
        s->tear_after_writes--;
    }

    if (page == 2) {
        s->pages[2][2] |= data[2];      /* lock bits only ever set */
        s->pages[2][3] |= data[3];
    } else if (page == 3) {
        for (int i = 0; i < 4; i++) {
            s->pages[3][i] |= data[i];  /* the capability container is one-time programmable */
        }
    } else {
        memcpy(s->pages[page], data, 4);
    }
    s->writes++;
    return NFC_OK;
}

static nfc_err_t transceive(nfc_xcvr_t *x, const uint8_t *tx, size_t tx_len,
                            uint8_t *rx, size_t rx_cap, size_t *rx_len)
{
    sim_ntag_t *s = (sim_ntag_t *)x;
    *rx_len = 0;
    s->exchanges++;
    if (s->reader_broken) {
        return NFC_ERR_IO;
    }
    if (!s->present || !s->selected) {
        return NFC_ERR_TIMEOUT;
    }
    if (s->silent_after == 0) {
        s->silent_after = -1;
        s->selected = false;            /* a missed frame leaves the tag needing re-selection */
        s->authed = false;
        return NFC_ERR_TIMEOUT;
    }
    if (s->silent_after > 0) {
        s->silent_after--;
    }
    if (tx_len < 1) {
        return nak(s);
    }

    switch (tx[0]) {
    case 0x60:  /* GET_VERSION */
        if (!s->supports_version || tx_len != 1 || rx_cap < 8) {
            return nak(s);
        }
        rx[0] = 0x00;
        rx[1] = 0x04;
        rx[2] = 0x04;
        rx[3] = 0x02;
        rx[4] = 0x01;
        rx[5] = 0x00;
        rx[6] = s->type == NTAG_213 ? 0x0F : (s->type == NTAG_215 ? 0x11 : 0x13);
        rx[7] = 0x03;
        *rx_len = 8;
        return NFC_OK;
    case 0x30:  /* READ */
        if (tx_len != 2) {
            return nak(s);
        }
        return do_read(s, tx[1], rx, rx_cap, rx_len);
    case 0xA2:  /* WRITE */
        if (tx_len != 6) {
            return nak(s);
        }
        return do_write(s, tx[1], tx + 2);
    case 0x1B:  /* PWD_AUTH */
        if (tx_len != 5 || rx_cap < 2 || memcmp(tx + 1, s->pages[s->cfg_page + 2], 4) != 0) {
            return nak(s);
        }
        s->authed = true;
        memcpy(rx, s->pages[s->cfg_page + 3], 2);
        *rx_len = 2;
        return NFC_OK;
    default:
        return nak(s);
    }
}

static nfc_err_t reselect(nfc_xcvr_t *x)
{
    sim_ntag_t *s = (sim_ntag_t *)x;
    s->reselects++;
    if (s->reader_broken) {
        return NFC_ERR_IO;
    }
    if (!s->present) {
        return NFC_ERR_TIMEOUT;
    }
    s->selected = true;
    s->authed = false;
    return NFC_OK;
}

void sim_ntag_init(sim_ntag_t *s, ntag_type_t type)
{
    memset(s, 0, sizeof(*s));
    s->xcvr.transceive = transceive;
    s->xcvr.reselect = reselect;
    s->type = type;
    s->present = true;
    s->selected = true;
    s->supports_version = true;
    s->tear_after_writes = -1;
    s->silent_after = -1;

    uint8_t cc_size;
    switch (type) {
    case NTAG_213:
        s->cfg_page = 0x29;
        cc_size = 0x12;
        break;
    case NTAG_216:
        s->cfg_page = 0xE3;
        cc_size = 0x6D;
        break;
    default:
        s->type = NTAG_215;
        s->cfg_page = 0x83;
        cc_size = 0x3E;
        break;
    }
    s->last_page = s->cfg_page + 3;

    /* UID 04 A1 B2 C3 D4 E5 F6 with its check bytes */
    static const uint8_t uid[7] = { 0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };
    memcpy(s->pages[0], uid, 3);
    s->pages[0][3] = 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
    memcpy(s->pages[1], uid + 3, 4);
    s->pages[2][0] = uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
    s->pages[2][1] = 0x48;

    const uint8_t cc[4] = { 0xE1, 0x10, cc_size, 0x00 };
    memcpy(s->pages[3], cc, 4);

    /* As delivered: an empty NDEF message, after a lock control TLV on the 213 and 216. */
    if (s->type == NTAG_215) {
        const uint8_t data[4] = { 0x03, 0x00, 0xFE, 0x00 };
        memcpy(s->pages[4], data, 4);
    } else {
        const uint8_t lock_tlv_213[4] = { 0x01, 0x03, 0xA0, 0x0C };
        const uint8_t lock_tlv_216[4] = { 0x01, 0x03, 0xE8, 0x0E };
        const uint8_t rest_213[4] = { 0x34, 0x03, 0x00, 0xFE };
        const uint8_t rest_216[4] = { 0x66, 0x03, 0x00, 0xFE };
        memcpy(s->pages[4], s->type == NTAG_213 ? lock_tlv_213 : lock_tlv_216, 4);
        memcpy(s->pages[5], s->type == NTAG_213 ? rest_213 : rest_216, 4);
    }

    const uint8_t cfg0[4] = { 0x04, 0x00, 0x00, 0xFF };
    const uint8_t cfg1[4] = { 0x00, 0x05, 0x00, 0x00 };
    const uint8_t pwd[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    memcpy(s->pages[s->cfg_page], cfg0, 4);
    memcpy(s->pages[s->cfg_page + 1], cfg1, 4);
    memcpy(s->pages[s->cfg_page + 2], pwd, 4);
}

nfc_xcvr_t *sim_ntag_xcvr(sim_ntag_t *s)
{
    return &s->xcvr;
}

void sim_ntag_present(sim_ntag_t *s)
{
    s->present = true;
    s->selected = true;
    s->authed = false;
}

uint8_t *sim_ntag_page(sim_ntag_t *s, uint8_t page)
{
    return s->pages[page];
}

void sim_ntag_set_ndef(sim_ntag_t *s, const uint8_t *msg, size_t len)
{
    uint8_t *area = s->pages[PAGE_DATA];
    const size_t cap = (size_t)(s->cfg_page - 1 - PAGE_DATA) * 4;
    memset(area, 0, cap);
    t2t_wrap_ndef(msg, len, area, cap);
}

void sim_ntag_protect(sim_ntag_t *s, const uint8_t pwd[4], const uint8_t pack[2], uint8_t auth0_page, bool prot_read)
{
    memcpy(s->pages[s->cfg_page + 2], pwd, 4);
    memcpy(s->pages[s->cfg_page + 3], pack, 2);
    s->pages[s->cfg_page][3] = auth0_page;
    if (prot_read) {
        s->pages[s->cfg_page + 1][0] |= 0x80;
    } else {
        s->pages[s->cfg_page + 1][0] &= (uint8_t)~0x80;
    }
    s->authed = false;
}

bool sim_ntag_get_ndef(const sim_ntag_t *s, const uint8_t **msg, size_t *len)
{
    const uint8_t *area = s->pages[PAGE_DATA];
    const size_t cap = (size_t)(s->cfg_page - 1 - PAGE_DATA) * 4;
    size_t off = 0, need = 0;
    if (t2t_find_ndef(area, cap, &off, len, &need) != T2T_NDEF_FOUND) {
        return false;
    }
    *msg = area + off;
    return true;
}
