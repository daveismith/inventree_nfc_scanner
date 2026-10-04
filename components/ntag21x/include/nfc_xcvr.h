/*
 * The seam between tag logic and whatever talks to the tag.
 *
 * ntag21x is written against this, so it runs unchanged over the PN532 on the board and over
 * the simulated tag in the host tests.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NFC_OK = 0,
    NFC_ERR_TIMEOUT,    /* no usable answer (silence, or one damaged in the air): the tag may have left */
    NFC_ERR_NAK,        /* the tag refused the command; it is back in IDLE and needs re-selecting */
    NFC_ERR_IO,         /* the reader itself failed */
} nfc_err_t;

typedef struct nfc_xcvr nfc_xcvr_t;

struct nfc_xcvr {
    /*
     * Send one Type 2 command to the selected tag and collect its answer. The transceiver adds
     * and checks the CRC. A command the tag answers with a bare ACK (WRITE) returns NFC_OK with
     * *rx_len == 0.
     */
    nfc_err_t (*transceive)(nfc_xcvr_t *x, const uint8_t *tx, size_t tx_len,
                            uint8_t *rx, size_t rx_cap, size_t *rx_len);

    /*
     * Select the same tag again after an error. NFC_ERR_TIMEOUT means it is gone. A tag loses
     * its authenticated state across this.
     */
    nfc_err_t (*reselect)(nfc_xcvr_t *x);
};

#ifdef __cplusplus
}
#endif
