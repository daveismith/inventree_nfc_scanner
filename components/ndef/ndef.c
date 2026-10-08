#include "ndef.h"

#include <string.h>

/* NDEF record header flags */
#define FLAG_MB  0x80
#define FLAG_ME  0x40
#define FLAG_CF  0x20
#define FLAG_SR  0x10
#define FLAG_IL  0x08
#define TNF_MASK 0x07

#define TNF_WELL_KNOWN 0x01

/* Type 2 Tag TLV tags */
#define TLV_NULL       0x00
#define TLV_LOCK_CTRL  0x01
#define TLV_MEM_CTRL   0x02
#define TLV_NDEF       0x03
#define TLV_PROPRIETARY 0xFD
#define TLV_TERMINATOR 0xFE

/* NFC Forum URI Record Type Definition, table 3: the identifier code is an index into this. */
static const char *const s_uri_prefix[] = {
    "", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:",
    "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://", "sftp://", "smb://", "nfs://",
    "ftp://", "dav://", "news:", "telnet://", "imap:", "rtsp://", "urn:", "pop:", "sip:",
    "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://", "tcpobex://", "irdaobex://",
    "file://", "urn:epc:id:", "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:",
    "urn:nfc:",
};

typedef struct {
    uint8_t flags;
    const uint8_t *type;
    size_t type_len;
    const uint8_t *payload;
    size_t payload_len;
} record_t;

/* Decode one record at `p`. Returns the bytes it occupies, or 0 if it does not fit in `len`. */
static size_t read_record(const uint8_t *p, size_t len, record_t *r)
{
    size_t i = 0;
    if (len < 3) {
        return 0;
    }
    r->flags = p[i++];
    r->type_len = p[i++];

    if (r->flags & FLAG_SR) {
        r->payload_len = p[i++];
    } else {
        if (len < i + 4) {
            return 0;
        }
        r->payload_len = ((size_t)p[i] << 24) | ((size_t)p[i + 1] << 16) | ((size_t)p[i + 2] << 8) | p[i + 3];
        i += 4;
    }

    size_t id_len = 0;
    if (r->flags & FLAG_IL) {
        if (len < i + 1) {
            return 0;
        }
        id_len = p[i++];
    }

    /* Checked piecewise so a hostile 32-bit payload length cannot wrap the sum. */
    if (r->type_len > len - i) {
        return 0;
    }
    r->type = p + i;
    i += r->type_len;
    if (id_len > len - i) {
        return 0;
    }
    i += id_len;
    if (r->payload_len > len - i) {
        return 0;
    }
    r->payload = p + i;
    i += r->payload_len;
    return i;
}

static bool utf8_valid(const uint8_t *s, size_t n);

static void take_uri(const record_t *r, ndef_info_t *out)
{
    if (r->payload_len < 1) {
        return;
    }
    const uint8_t code = r->payload[0];
    const char *prefix = code < sizeof(s_uri_prefix) / sizeof(s_uri_prefix[0]) ? s_uri_prefix[code] : "";
    const size_t prefix_len = strlen(prefix);
    const size_t rest = r->payload_len - 1;
    if (prefix_len + rest > NDEF_URI_MAX) {
        return;
    }
    memcpy(out->uri, prefix, prefix_len);
    if (!utf8_valid(r->payload + 1, rest)) {
        out->rejected = true;
        return;
    }
    memcpy(out->uri + prefix_len, r->payload + 1, rest);
    out->uri[prefix_len + rest] = '\0';
    out->has_uri = true;
}

/* Well-formed UTF-8, with no NUL: what can go into a JSON string and a text field. */
static bool utf8_valid(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        const uint8_t c = s[i];
        size_t more;
        uint32_t cp;
        if (c == 0) {
            return false;
        } else if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
            more = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            more = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
            more = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        for (size_t k = 1; k <= more; k++) {
            if (i + k >= n || (s[i + k] & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if ((more == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (more == 3 && (cp < 0x10000 || cp > 0x10FFFF))) {
            return false;
        }
        i += more + 1;
    }
    return true;
}

static void take_text(const record_t *r, ndef_info_t *out)
{
    if (r->payload_len < 1) {
        return;
    }
    const uint8_t status = r->payload[0];
    if (status & 0x80) {
        return;                         /* UTF-16: not something a barcode field holds */
    }
    const size_t lang_len = status & 0x3F;
    if (1 + lang_len > r->payload_len) {
        return;
    }
    const size_t text_len = r->payload_len - 1 - lang_len;
    if (text_len > NDEF_TEXT_MAX) {
        return;
    }
    if (!utf8_valid(r->payload + 1 + lang_len, text_len)) {
        out->rejected = true;
        return;
    }
    memcpy(out->text, r->payload + 1 + lang_len, text_len);
    out->text[text_len] = '\0';
    out->has_text = true;
}

bool ndef_parse_message(const uint8_t *msg, size_t len, ndef_info_t *out)
{
    ndef_info_t scratch;
    if (out == NULL) {
        out = &scratch;
    }
    memset(out, 0, sizeof(*out));
    if (len == 0) {
        return true;                    /* an empty message is what a formatted blank tag holds */
    }

    size_t pos = 0;
    bool ended = false;
    while (pos < len) {
        record_t r;
        const size_t used = read_record(msg + pos, len - pos, &r);
        if (used == 0 || ended || (r.flags & FLAG_CF)) {
            return false;
        }
        /* MB on the first record and only there. */
        if (((r.flags & FLAG_MB) != 0) != (pos == 0)) {
            return false;
        }
        ended = (r.flags & FLAG_ME) != 0;

        if ((r.flags & TNF_MASK) == TNF_WELL_KNOWN && r.type_len == 1) {
            if (r.type[0] == 'U' && !out->has_uri) {
                take_uri(&r, out);
            } else if (r.type[0] == 'T' && !out->has_text) {
                take_text(&r, out);
            }
        }
        out->records++;
        pos += used;
    }
    return ended;
}

bool ndef_message_valid(const uint8_t *msg, size_t len)
{
    ndef_info_t info;
    return len > 0 && ndef_parse_message(msg, len, &info) && !info.rejected;
}

bool ndef_message_is_empty(const uint8_t *msg, size_t len)
{
    size_t pos = 0;
    while (pos < len) {
        record_t r;
        const size_t used = read_record(msg + pos, len - pos, &r);
        if (used == 0 || (r.flags & TNF_MASK) != 0) {
            return false;
        }
        pos += used;
    }
    return true;
}

t2t_find_t t2t_find_ndef(const uint8_t *area, size_t area_len, size_t *off, size_t *len, size_t *need)
{
    size_t i = 0;
    while (i < area_len) {
        const uint8_t tag = area[i];
        if (tag == TLV_NULL) {
            i++;
            continue;
        }
        if (tag == TLV_TERMINATOR) {
            return T2T_NDEF_NONE;
        }
        if (tag != TLV_LOCK_CTRL && tag != TLV_MEM_CTRL && tag != TLV_NDEF && tag != TLV_PROPRIETARY) {
            return T2T_NDEF_MALFORMED;
        }

        /* Length: one byte, or 0xFF followed by two big-endian bytes. */
        if (i + 2 > area_len) {
            *need = i + 4;
            return T2T_NDEF_MORE;
        }
        size_t value_len = area[i + 1];
        size_t header = 2;
        if (value_len == 0xFF) {
            if (i + 4 > area_len) {
                *need = i + 4;
                return T2T_NDEF_MORE;
            }
            value_len = ((size_t)area[i + 2] << 8) | area[i + 3];
            header = 4;
        }

        if (i + header + value_len > area_len) {
            *need = i + header + value_len;
            return T2T_NDEF_MORE;
        }
        if (tag == TLV_NDEF) {
            *off = i + header;
            *len = value_len;
            return T2T_NDEF_FOUND;
        }
        i += header + value_len;
    }
    /* Ran off the end without a terminator: there may be more further on. */
    *need = area_len + 1;
    return T2T_NDEF_MORE;
}

size_t t2t_tlv_overhead(size_t msg_len)
{
    return (msg_len < 0xFF ? 2 : 4) + 1;
}

size_t t2t_wrap_ndef(const uint8_t *msg, size_t msg_len, uint8_t *out, size_t cap)
{
    if (msg_len > 0xFFFF || msg_len + t2t_tlv_overhead(msg_len) > cap) {
        return 0;
    }
    size_t i = 0;
    out[i++] = TLV_NDEF;
    if (msg_len < 0xFF) {
        out[i++] = (uint8_t)msg_len;
    } else {
        out[i++] = 0xFF;
        out[i++] = (uint8_t)(msg_len >> 8);
        out[i++] = (uint8_t)msg_len;
    }
    if (msg_len) {
        memcpy(out + i, msg, msg_len);
        i += msg_len;
    }
    out[i++] = TLV_TERMINATOR;
    return i;
}
