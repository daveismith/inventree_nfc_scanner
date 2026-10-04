/*
 * An NTAG213/215/216 in memory, behind the same nfc_xcvr_t the PN532 presents.
 *
 * It models what the firmware's correctness depends on: the page map, password protection
 * (PWD_AUTH, AUTH0, PROT), the one-time-programmable capability container and lock bits, the
 * tag dropping to IDLE after a NAK, and losing authentication when re-selected. It can be
 * taken out of the field after a set number of writes, and made to miss exchanges.
 *
 * Used by the host tests and by the host simulator.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nfc_xcvr.h"
#include "ntag21x.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_NTAG_PAGES 231      /* NTAG216: pages 00h to E6h */

typedef struct {
    nfc_xcvr_t xcvr;            /* first, so the vtable pointer is the tag */
    ntag_type_t type;
    uint8_t cfg_page;
    uint8_t last_page;
    uint8_t pages[SIM_NTAG_PAGES][4];

    bool present;               /* in the field */
    bool selected;              /* false after a NAK, until re-selected */
    bool authed;
    bool supports_version;      /* false: answers like a tag that is not an NTAG21x */
    bool reader_broken;         /* every exchange fails with NFC_ERR_IO */

    int tear_after_writes;      /* < 0: never. n: the write after n more leaves the field instead */
    int silent_after;           /* < 0: never. n: after n more exchanges, one goes unanswered */

    unsigned writes;
    unsigned exchanges;
    unsigned reselects;
} sim_ntag_t;

/* A tag as delivered: formatted, empty, unprotected, in the field and selected. */
void sim_ntag_init(sim_ntag_t *s, ntag_type_t type);

nfc_xcvr_t *sim_ntag_xcvr(sim_ntag_t *s);

/* Put the tag back in the field after it was torn away. */
void sim_ntag_present(sim_ntag_t *s);

/* Set memory directly, as though the tag had been written elsewhere. */
void sim_ntag_set_ndef(sim_ntag_t *s, const uint8_t *msg, size_t len);
void sim_ntag_protect(sim_ntag_t *s, const uint8_t pwd[4], const uint8_t pack[2], uint8_t auth0, bool prot_read);

/* The message in memory. Returns false when the data area holds no well-formed NDEF TLV. */
bool sim_ntag_get_ndef(const sim_ntag_t *s, const uint8_t **msg, size_t *len);

uint8_t *sim_ntag_page(sim_ntag_t *s, uint8_t page);

#ifdef __cplusplus
}
#endif
