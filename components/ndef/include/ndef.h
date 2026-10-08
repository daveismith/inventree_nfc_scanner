/*
 * NDEF message parsing and the NFC Forum Type 2 Tag TLV wrapping around it.
 *
 * Pure C with no ESP-IDF dependencies, so it builds and is tested on the host.
 *
 * The firmware never builds NDEF records: the page sends a complete message and the firmware
 * wraps it in the Type 2 TLV. Parsing exists for lookup, which needs the URI and Text records.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NDEF_TEXT_MAX 127
#define NDEF_URI_MAX  255

typedef struct {
    size_t records;
    bool has_text;
    char text[NDEF_TEXT_MAX + 1];   /* first UTF-8 Text record, NUL terminated */
    bool has_uri;
    bool rejected;              /* a Text or URI record was there but not usable (not UTF-8) */
    char uri[NDEF_URI_MAX + 1];     /* first URI record, prefix code expanded */
} ndef_info_t;

/*
 * Walk an NDEF message and pull out the first URI and first Text record. Returns false when
 * the message is malformed: a record overruns the buffer, MB/ME are misplaced, or a record is
 * chunked (not supported). A record that is well-formed but too long for the buffers above, or
 * a UTF-16 Text record, leaves its has_* flag false without failing the parse.
 */
bool ndef_parse_message(const uint8_t *msg, size_t len, ndef_info_t *out);

/* Structural check only: what `program` runs on the bytes the page sent. */
bool ndef_message_valid(const uint8_t *msg, size_t len);

/*
 * A message with nothing in it: no bytes at all, or only Empty records (TNF 0), which is what
 * a phone leaves when it "erases" a tag. Anything that cannot be parsed is not empty.
 */
bool ndef_message_is_empty(const uint8_t *msg, size_t len);

typedef enum {
    T2T_NDEF_FOUND,      /* an NDEF Message TLV with a complete value */
    T2T_NDEF_NONE,       /* terminator reached without one */
    T2T_NDEF_MORE,       /* undecided within `area_len`: read `*need` bytes in total and retry */
    T2T_NDEF_MALFORMED,
} t2t_find_t;

/*
 * Find the NDEF Message TLV (0x03) in a Type 2 tag's data area, skipping NULL, lock control,
 * memory control and proprietary TLVs. `area` starts at page 4. On T2T_NDEF_FOUND, `*off` and
 * `*len` locate the message (len may be 0: an empty, formatted tag). On T2T_NDEF_MORE, `*need`
 * is how many bytes of the area are required before the answer is known; a caller that already
 * passed the whole area treats that as "none".
 */
t2t_find_t t2t_find_ndef(const uint8_t *area, size_t area_len, size_t *off, size_t *len, size_t *need);

/* Bytes the TLV adds around a message of `msg_len`: T, L (1 or 3 bytes) and the terminator. */
size_t t2t_tlv_overhead(size_t msg_len);

/*
 * Write `03 <len> <msg> FE` to `out`. Returns the number of bytes written, or 0 when it does
 * not fit in `cap` or the message is longer than a TLV can describe.
 */
size_t t2t_wrap_ndef(const uint8_t *msg, size_t msg_len, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
