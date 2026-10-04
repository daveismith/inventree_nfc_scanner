#include "test_util.h"

#include <string.h>

#include "unity.h"

size_t make_inventree_ndef(uint8_t *out, size_t cap, const char *uri_rest, const char *text)
{
    const size_t uri_len = strlen(uri_rest);
    const size_t text_len = strlen(text);
    TEST_ASSERT_TRUE(1 + uri_len < 256 && 3 + text_len < 256);
    TEST_ASSERT_TRUE(4 + 1 + uri_len + 4 + 3 + text_len <= cap);

    size_t i = 0;
    out[i++] = 0x91;                        /* MB, SR, well-known */
    out[i++] = 1;
    out[i++] = (uint8_t)(1 + uri_len);
    out[i++] = 'U';
    out[i++] = 0x04;                        /* https:// */
    memcpy(out + i, uri_rest, uri_len);
    i += uri_len;

    out[i++] = 0x51;                        /* ME, SR, well-known */
    out[i++] = 1;
    out[i++] = (uint8_t)(3 + text_len);
    out[i++] = 'T';
    out[i++] = 0x02;                        /* UTF-8, two-letter language */
    out[i++] = 'e';
    out[i++] = 'n';
    memcpy(out + i, text, text_len);
    i += text_len;
    return i;
}

size_t make_text_ndef(uint8_t *out, size_t cap, size_t text_len)
{
    const size_t payload = 3 + text_len;
    size_t i = 0;
    if (payload < 256) {
        TEST_ASSERT_TRUE(4 + payload <= cap);
        out[i++] = 0xD1;                    /* MB, ME, SR, well-known */
        out[i++] = 1;
        out[i++] = (uint8_t)payload;
    } else {
        TEST_ASSERT_TRUE(7 + payload <= cap);
        out[i++] = 0xC1;                    /* MB, ME, well-known, four-byte length */
        out[i++] = 1;
        out[i++] = 0;
        out[i++] = 0;
        out[i++] = (uint8_t)(payload >> 8);
        out[i++] = (uint8_t)payload;
    }
    out[i++] = 'T';
    out[i++] = 0x02;
    out[i++] = 'e';
    out[i++] = 'n';
    for (size_t k = 0; k < text_len; k++) {
        out[i++] = (uint8_t)('a' + k % 26);
    }
    return i;
}
