#include <string.h>

#include "unity.h"

#include "pn532_frame.h"
#include "test_util.h"

/* Reference frames from the PN532 user manual (UM0701-02), section 7.2. */
static void test_build_known_commands(void)
{
    uint8_t out[32];

    const uint8_t get_firmware_version[] = { 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00 };
    TEST_ASSERT_EQUAL(sizeof(get_firmware_version), pn532_frame_build(0x02, NULL, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(get_firmware_version, out, sizeof(get_firmware_version));

    const uint8_t sam_params[] = { 0x01, 0x14, 0x01 };
    const uint8_t sam_configuration[] = { 0x00, 0x00, 0xFF, 0x05, 0xFB, 0xD4, 0x14, 0x01, 0x14, 0x01, 0x02, 0x00 };
    TEST_ASSERT_EQUAL(sizeof(sam_configuration), pn532_frame_build(0x14, sam_params, sizeof(sam_params), out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(sam_configuration, out, sizeof(sam_configuration));
}

static void test_build_refuses_what_does_not_fit(void)
{
    uint8_t out[300];
    uint8_t params[260] = { 0 };
    TEST_ASSERT_EQUAL(0, pn532_frame_build(0x42, params, 4, out, 12));
    TEST_ASSERT_EQUAL(13, pn532_frame_build(0x42, params, 4, out, 13));
    TEST_ASSERT_EQUAL(0, pn532_frame_build(0x42, params, PN532_MAX_PARAMS + 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(PN532_MAX_PARAMS + 9, pn532_frame_build(0x42, params, PN532_MAX_PARAMS, out, sizeof(out)));
}

static void test_parse_ack_and_nack(void)
{
    TEST_ASSERT_EQUAL(PN532_FRAME_ACK, pn532_frame_parse(PN532_ACK_FRAME, PN532_ACK_LEN, NULL, NULL));
    const uint8_t nack[] = { 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00 };
    TEST_ASSERT_EQUAL(PN532_FRAME_NACK, pn532_frame_parse(nack, sizeof(nack), NULL, NULL));
}

static void test_parse_response(void)
{
    /* GetFirmwareVersion from a PN532 v1.6 */
    const uint8_t rsp[] = { 0x00, 0x00, 0xFF, 0x06, 0xFA, 0xD5, 0x03, 0x32, 0x01, 0x06, 0x07, 0xE8, 0x00 };
    const uint8_t *payload = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL(PN532_FRAME_DATA, pn532_frame_parse(rsp, sizeof(rsp), &payload, &n));
    TEST_ASSERT_EQUAL(5, n);
    const uint8_t want[] = { 0x03, 0x32, 0x01, 0x06, 0x07 };
    TEST_ASSERT_EQUAL_MEMORY(want, payload, sizeof(want));

    /* The I2C ready byte, or a shorter preamble, in front of it */
    uint8_t shifted[1 + sizeof(rsp)] = { 0x01 };
    memcpy(shifted + 1, rsp, sizeof(rsp));
    TEST_ASSERT_EQUAL(PN532_FRAME_DATA, pn532_frame_parse(shifted, sizeof(shifted), &payload, &n));
    TEST_ASSERT_EQUAL(5, n);
    TEST_ASSERT_EQUAL(PN532_FRAME_DATA, pn532_frame_parse(rsp + 1, sizeof(rsp) - 1, &payload, &n));
}

static void test_parse_rejects_damage(void)
{
    const uint8_t good[] = { 0x00, 0x00, 0xFF, 0x06, 0xFA, 0xD5, 0x03, 0x32, 0x01, 0x06, 0x07, 0xE8, 0x00 };
    uint8_t bad[sizeof(good)];

    memcpy(bad, good, sizeof(good));
    bad[4] ^= 0x01;                             /* length checksum */
    TEST_ASSERT_EQUAL(PN532_FRAME_INVALID, pn532_frame_parse(bad, sizeof(bad), NULL, NULL));

    memcpy(bad, good, sizeof(good));
    bad[8] ^= 0x10;                             /* a data byte */
    TEST_ASSERT_EQUAL(PN532_FRAME_INVALID, pn532_frame_parse(bad, sizeof(bad), NULL, NULL));

    memcpy(bad, good, sizeof(good));
    bad[5] = 0xD4;                              /* our own direction byte */
    bad[11] = 0xE9;
    TEST_ASSERT_EQUAL(PN532_FRAME_INVALID, pn532_frame_parse(bad, sizeof(bad), NULL, NULL));

    for (size_t cut = 0; cut < sizeof(good) - 1; cut++) {
        TEST_ASSERT_EQUAL(PN532_FRAME_INVALID, pn532_frame_parse(good, cut, NULL, NULL));
    }

    const uint8_t all_ones[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL(PN532_FRAME_INVALID, pn532_frame_parse(all_ones, sizeof(all_ones), NULL, NULL));
}

static void test_parse_error_frame(void)
{
    const uint8_t err[] = { 0x00, 0x00, 0xFF, 0x01, 0xFF, 0x7F, 0x81, 0x00 };
    TEST_ASSERT_EQUAL(PN532_FRAME_ERROR, pn532_frame_parse(err, sizeof(err), NULL, NULL));
}

static void test_build_then_parse_is_not_a_response(void)
{
    /* A command frame carries D4, so reading our own bytes back is never taken for an answer. */
    uint8_t out[32];
    const size_t n = pn532_frame_build(0x4A, (const uint8_t[]){ 0x02, 0x00 }, 2, out, sizeof(out));
    TEST_ASSERT_EQUAL(PN532_FRAME_INVALID, pn532_frame_parse(out, n, NULL, NULL));
}

void run_pn532_frame_tests(void)
{
    RUN_TEST(test_build_known_commands);
    RUN_TEST(test_build_refuses_what_does_not_fit);
    RUN_TEST(test_parse_ack_and_nack);
    RUN_TEST(test_parse_response);
    RUN_TEST(test_parse_rejects_damage);
    RUN_TEST(test_parse_error_frame);
    RUN_TEST(test_build_then_parse_is_not_a_response);
}
