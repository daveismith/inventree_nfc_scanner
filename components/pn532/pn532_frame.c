#include "pn532_frame.h"

#include <string.h>

const uint8_t PN532_ACK_FRAME[PN532_ACK_LEN] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };

size_t pn532_frame_build(uint8_t cmd, const uint8_t *params, size_t params_len, uint8_t *out, size_t cap)
{
    if (params_len > PN532_MAX_PARAMS || params_len + PN532_FRAME_OVERHEAD + 1 > cap) {
        return 0;
    }
    const uint8_t len = (uint8_t)(params_len + 2);      /* TFI + command + parameters */
    size_t i = 0;
    out[i++] = 0x00;
    out[i++] = 0x00;
    out[i++] = 0xFF;
    out[i++] = len;
    out[i++] = (uint8_t)(0x100 - len);

    uint8_t sum = PN532_TFI_HOST_TO_PN532 + cmd;
    out[i++] = PN532_TFI_HOST_TO_PN532;
    out[i++] = cmd;
    for (size_t k = 0; k < params_len; k++) {
        out[i++] = params[k];
        sum += params[k];
    }
    out[i++] = (uint8_t)(0x100 - sum);
    out[i++] = 0x00;
    return i;
}

pn532_frame_kind_t pn532_frame_parse(const uint8_t *buf, size_t len, const uint8_t **payload, size_t *payload_len)
{
    /* The start code is 00 FF; any number of 00 preamble bytes may come before it. */
    size_t s = 0;
    while (s + 1 < len && !(buf[s] == 0x00 && buf[s + 1] == 0xFF)) {
        s++;
    }
    if (s + 4 > len) {
        return PN532_FRAME_INVALID;
    }
    const uint8_t flen = buf[s + 2];
    const uint8_t lcs = buf[s + 3];

    if (flen == 0x00 && lcs == 0xFF) {
        return PN532_FRAME_ACK;
    }
    if (flen == 0xFF && lcs == 0x00) {
        return PN532_FRAME_NACK;
    }
    if ((uint8_t)(flen + lcs) != 0 || flen == 0) {
        return PN532_FRAME_INVALID;
    }
    /* LEN bytes of TFI and data, then DCS */
    if (s + 4 + (size_t)flen + 1 > len) {
        return PN532_FRAME_INVALID;
    }
    const uint8_t *body = buf + s + 4;
    uint8_t sum = 0;
    for (size_t k = 0; k <= flen; k++) {
        sum += body[k];
    }
    if (sum != 0) {
        return PN532_FRAME_INVALID;
    }

    if (flen == 1 && body[0] == 0x7F) {
        return PN532_FRAME_ERROR;
    }
    if (body[0] != PN532_TFI_PN532_TO_HOST || flen < 2) {
        return PN532_FRAME_INVALID;
    }
    if (payload) {
        *payload = body + 1;
    }
    if (payload_len) {
        *payload_len = (size_t)flen - 1;
    }
    return PN532_FRAME_DATA;
}
