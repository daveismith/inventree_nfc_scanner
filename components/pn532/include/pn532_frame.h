/*
 * PN532 host-link frames (user manual UM0701-02, section 6.2.1): the normal information
 * frame, ACK, NACK and the application-level error frame. Pure C, tested on the host.
 *
 *   00 00 FF LEN LCS TFI PD0 .. PDn DCS 00
 *
 * LEN counts TFI and the data; LCS makes LEN + LCS zero; DCS makes TFI + data + DCS zero.
 * Extended frames are not used: nothing this firmware asks for exceeds a normal frame.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PN532_TFI_HOST_TO_PN532 0xD4
#define PN532_TFI_PN532_TO_HOST 0xD5

#define PN532_FRAME_OVERHEAD    8       /* everything but the command byte and its parameters */
#define PN532_MAX_PARAMS        253     /* LEN is one byte and also counts TFI and the command */
#define PN532_ACK_LEN           6

extern const uint8_t PN532_ACK_FRAME[PN532_ACK_LEN];

typedef enum {
    PN532_FRAME_DATA,       /* a response; payload starts at the response code */
    PN532_FRAME_ACK,
    PN532_FRAME_NACK,
    PN532_FRAME_ERROR,      /* the PN532 did not understand the command */
    PN532_FRAME_INVALID,    /* no start code, bad checksum, truncated, or not from the PN532 */
} pn532_frame_kind_t;

/* Build a command frame. Returns its length, or 0 when it does not fit. */
size_t pn532_frame_build(uint8_t cmd, const uint8_t *params, size_t params_len, uint8_t *out, size_t cap);

/*
 * Parse what was read from the PN532, starting at or before the preamble. For a data frame,
 * `*payload` points at the response code (command + 1) and `*payload_len` counts it and the
 * bytes after it.
 */
pn532_frame_kind_t pn532_frame_parse(const uint8_t *buf, size_t len, const uint8_t **payload, size_t *payload_len);

#ifdef __cplusplus
}
#endif
