#include <string.h>

#include "unity.h"

#include "ndef.h"
#include "test_util.h"

static void test_inventree_message_parses(void)
{
    uint8_t msg[128];
    const size_t n = make_inventree_ndef(msg, sizeof(msg), "inventree.example/web/stock/location/42", "INV-SL42");

    ndef_info_t info;
    TEST_ASSERT_TRUE(ndef_parse_message(msg, n, &info));
    TEST_ASSERT_EQUAL(2, info.records);
    TEST_ASSERT_TRUE(info.has_uri);
    TEST_ASSERT_EQUAL_STRING("https://inventree.example/web/stock/location/42", info.uri);
    TEST_ASSERT_TRUE(info.has_text);
    TEST_ASSERT_EQUAL_STRING("INV-SL42", info.text);
    TEST_ASSERT_TRUE(ndef_message_valid(msg, n));
}

static void test_empty_message_is_fine_but_not_programmable(void)
{
    ndef_info_t info;
    TEST_ASSERT_TRUE(ndef_parse_message(NULL, 0, &info));
    TEST_ASSERT_EQUAL(0, info.records);
    TEST_ASSERT_FALSE(ndef_message_valid(NULL, 0));
}

static void test_truncated_message_is_rejected(void)
{
    uint8_t msg[128];
    const size_t n = make_inventree_ndef(msg, sizeof(msg), "h/1", "INV-SL1");
    for (size_t cut = 1; cut < n; cut++) {
        TEST_ASSERT_FALSE_MESSAGE(ndef_message_valid(msg, cut), "a cut message must not validate");
    }
}

static void test_message_flags_are_checked(void)
{
    uint8_t msg[128];
    const size_t n = make_inventree_ndef(msg, sizeof(msg), "h/1", "INV-SL1");
    const size_t second = 4 + 1 + 3;            /* header, prefix code, "h/1" */

    uint8_t bad[128];
    memcpy(bad, msg, n);
    bad[second] &= (uint8_t)~0x40;              /* no ME anywhere */
    TEST_ASSERT_FALSE(ndef_message_valid(bad, n));

    memcpy(bad, msg, n);
    bad[second] |= 0x80;                        /* MB on the second record */
    TEST_ASSERT_FALSE(ndef_message_valid(bad, n));

    memcpy(bad, msg, n);
    bad[0] &= (uint8_t)~0x80;                   /* no MB on the first */
    TEST_ASSERT_FALSE(ndef_message_valid(bad, n));

    memcpy(bad, msg, n);
    bad[0] |= 0x20;                             /* chunked */
    TEST_ASSERT_FALSE(ndef_message_valid(bad, n));

    memcpy(bad, msg, n);
    bad[0] |= 0x40;                             /* ME on the first, with a record after it */
    TEST_ASSERT_FALSE(ndef_message_valid(bad, n));
}

static void test_long_record_and_oversized_fields(void)
{
    uint8_t msg[600];
    size_t n = make_text_ndef(msg, sizeof(msg), 100);
    ndef_info_t info;
    TEST_ASSERT_TRUE(ndef_parse_message(msg, n, &info));
    TEST_ASSERT_TRUE(info.has_text);
    TEST_ASSERT_EQUAL(100, strlen(info.text));

    /* Four-byte payload length; the text is too long to keep, and that is not an error. */
    n = make_text_ndef(msg, sizeof(msg), 400);
    TEST_ASSERT_TRUE(ndef_parse_message(msg, n, &info));
    TEST_ASSERT_EQUAL(1, info.records);
    TEST_ASSERT_FALSE(info.has_text);

    /* A payload length that claims far more than is there must not be followed. */
    const uint8_t huge[] = { 0xC1, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 'T', 0x02 };
    TEST_ASSERT_FALSE(ndef_message_valid(huge, sizeof(huge)));
}

static void test_utf16_text_and_unknown_uri_prefix(void)
{
    const uint8_t utf16[] = { 0xD1, 0x01, 0x05, 'T', 0x82, 'e', 'n', 0x00, 'A' };
    ndef_info_t info;
    TEST_ASSERT_TRUE(ndef_parse_message(utf16, sizeof(utf16), &info));
    TEST_ASSERT_FALSE(info.has_text);

    const uint8_t odd_uri[] = { 0xD1, 0x01, 0x04, 'U', 0x7F, 'a', ':', 'b' };
    TEST_ASSERT_TRUE(ndef_parse_message(odd_uri, sizeof(odd_uri), &info));
    TEST_ASSERT_TRUE(info.has_uri);
    TEST_ASSERT_EQUAL_STRING("a:b", info.uri);
}

static void test_empty_records_count_as_empty(void)
{
    const uint8_t erased[] = { 0xD0, 0x00, 0x00 };          /* one Empty record: a phone's "erase" */
    TEST_ASSERT_TRUE(ndef_message_is_empty(NULL, 0));
    TEST_ASSERT_TRUE(ndef_message_is_empty(erased, sizeof(erased)));

    uint8_t msg[64];
    const size_t n = make_inventree_ndef(msg, sizeof(msg), "h/1", "INV-SL1");
    TEST_ASSERT_FALSE(ndef_message_is_empty(msg, n));
    TEST_ASSERT_FALSE(ndef_message_is_empty(msg, n - 1));    /* unreadable is not empty */
    const uint8_t junk[] = { 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_FALSE(ndef_message_is_empty(junk, sizeof(junk)));
}

static void test_tlv_round_trip_short_and_long(void)
{
    uint8_t msg[600], area[700];
    for (size_t text_len = 0; text_len <= 500; text_len += 125) {
        const size_t n = make_text_ndef(msg, sizeof(msg), text_len);
        const size_t wrapped = t2t_wrap_ndef(msg, n, area, sizeof(area));
        TEST_ASSERT_EQUAL(n + t2t_tlv_overhead(n), wrapped);
        TEST_ASSERT_EQUAL(n < 255 ? 3 : 5, t2t_tlv_overhead(n));
        TEST_ASSERT_EQUAL_HEX8(0xFE, area[wrapped - 1]);

        size_t off = 0, len = 0, need = 0;
        TEST_ASSERT_EQUAL(T2T_NDEF_FOUND, t2t_find_ndef(area, wrapped, &off, &len, &need));
        TEST_ASSERT_EQUAL(n, len);
        TEST_ASSERT_EQUAL_MEMORY(msg, area + off, n);
    }
    TEST_ASSERT_EQUAL(0, t2t_wrap_ndef(msg, 100, area, 102));       /* one byte short */
}

static void test_tlv_find_skips_other_tlvs(void)
{
    /* What an NTAG213 holds when delivered: a lock control TLV, then an empty message. */
    const uint8_t factory_213[] = { 0x01, 0x03, 0xA0, 0x0C, 0x34, 0x03, 0x00, 0xFE };
    size_t off = 0, len = 99, need = 0;
    TEST_ASSERT_EQUAL(T2T_NDEF_FOUND, t2t_find_ndef(factory_213, sizeof(factory_213), &off, &len, &need));
    TEST_ASSERT_EQUAL(7, off);
    TEST_ASSERT_EQUAL(0, len);

    const uint8_t nulls[] = { 0x00, 0x00, 0x03, 0x02, 0xAA, 0xBB, 0xFE };
    TEST_ASSERT_EQUAL(T2T_NDEF_FOUND, t2t_find_ndef(nulls, sizeof(nulls), &off, &len, &need));
    TEST_ASSERT_EQUAL(4, off);
    TEST_ASSERT_EQUAL(2, len);

    const uint8_t only_terminator[] = { 0xFE, 0x03, 0x01, 0x00 };
    TEST_ASSERT_EQUAL(T2T_NDEF_NONE, t2t_find_ndef(only_terminator, sizeof(only_terminator), &off, &len, &need));

    const uint8_t junk[] = { 0x44, 0x03, 0x00, 0xFE };
    TEST_ASSERT_EQUAL(T2T_NDEF_MALFORMED, t2t_find_ndef(junk, sizeof(junk), &off, &len, &need));
}

static void test_tlv_find_asks_for_more(void)
{
    uint8_t msg[400], area[420];
    const size_t n = make_text_ndef(msg, sizeof(msg), 300);
    const size_t wrapped = t2t_wrap_ndef(msg, n, area, sizeof(area));

    size_t off = 0, len = 0, need = 0;
    TEST_ASSERT_EQUAL(T2T_NDEF_MORE, t2t_find_ndef(area, 8, &off, &len, &need));
    TEST_ASSERT_EQUAL(4 + n, need);
    TEST_ASSERT_EQUAL(T2T_NDEF_FOUND, t2t_find_ndef(area, need, &off, &len, &need));
    TEST_ASSERT_TRUE(wrapped > need);

    /* A length field cut in half, and a data area of nothing but NULL TLVs. */
    TEST_ASSERT_EQUAL(T2T_NDEF_MORE, t2t_find_ndef(area, 3, &off, &len, &need));
    const uint8_t zeros[8] = { 0 };
    TEST_ASSERT_EQUAL(T2T_NDEF_MORE, t2t_find_ndef(zeros, sizeof(zeros), &off, &len, &need));
    TEST_ASSERT_EQUAL(9, need);
}

/* What comes off a tag goes into JSON strings and a text field: only well-formed UTF-8 does. */
static void test_text_and_uri_must_be_utf8(void)
{
    uint8_t msg[128];
    ndef_info_t info;
    const size_t n = make_inventree_ndef(msg, sizeof(msg), "h/1", "INV-SL1");
    TEST_ASSERT_TRUE(ndef_parse_message(msg, n, &info));
    TEST_ASSERT_TRUE(info.has_text && info.has_uri);

    uint8_t bad[128];
    memcpy(bad, msg, n);
    bad[n - 1] = 0xFF;                  /* the text's last byte: not UTF-8 */
    TEST_ASSERT_TRUE(ndef_parse_message(bad, n, &info));
    TEST_ASSERT_FALSE(info.has_text);
    TEST_ASSERT_TRUE(info.has_uri);

    memcpy(bad, msg, n);
    bad[n - 1] = 0x00;                  /* an embedded NUL */
    TEST_ASSERT_TRUE(ndef_parse_message(bad, n, &info));
    TEST_ASSERT_FALSE(info.has_text);

    /* Two-byte UTF-8 in the text is fine. */
    const size_t m = make_inventree_ndef(msg, sizeof(msg), "h/1", "INV-SL1\xC3\xA9");
    TEST_ASSERT_TRUE(ndef_parse_message(msg, m, &info));
    TEST_ASSERT_TRUE(info.has_text);
    TEST_ASSERT_EQUAL_STRING("INV-SL1\xC3\xA9", info.text);
}

void run_ndef_tests(void)
{
    RUN_TEST(test_text_and_uri_must_be_utf8);
    RUN_TEST(test_inventree_message_parses);
    RUN_TEST(test_empty_message_is_fine_but_not_programmable);
    RUN_TEST(test_truncated_message_is_rejected);
    RUN_TEST(test_message_flags_are_checked);
    RUN_TEST(test_long_record_and_oversized_fields);
    RUN_TEST(test_utf16_text_and_unknown_uri_prefix);
    RUN_TEST(test_empty_records_count_as_empty);
    RUN_TEST(test_tlv_round_trip_short_and_long);
    RUN_TEST(test_tlv_find_skips_other_tlvs);
    RUN_TEST(test_tlv_find_asks_for_more);
}
