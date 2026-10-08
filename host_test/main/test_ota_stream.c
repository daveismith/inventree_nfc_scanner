/* The serial update session (components/ota_stream), with a backend that keeps the bytes. */
#include <string.h>

#include "unity.h"

#include "ota_stream.h"
#include "test_util.h"

static ota_stream_t s;
static uint8_t s_written[64];
static size_t s_len;
static int s_aborts, s_begins;
static bool s_digest_ok, s_fail_write;

static bool b_begin(void *ctx, uint32_t size, const char **detail)
{
    (void)ctx;
    if (size > sizeof(s_written)) {
        *detail = "size: larger than the application slot";
        return false;
    }
    s_begins++;
    s_len = 0;
    return true;
}

static bool b_write(void *ctx, const uint8_t *data, size_t len, const char **detail)
{
    (void)ctx;
    if (s_fail_write) {
        *detail = "flash write failed";
        return false;
    }
    memcpy(s_written + s_len, data, len);
    s_len += len;
    return true;
}

static bool b_finish(void *ctx, const uint8_t sha256[32], const char **detail)
{
    (void)ctx;
    (void)sha256;
    if (!s_digest_ok) {
        *detail = "sha256 does not match";
    }
    return s_digest_ok;
}

static void b_abort(void *ctx)
{
    (void)ctx;
    s_aborts++;
}

static void start(void)
{
    const ota_stream_backend_t backend = { .begin = b_begin, .write = b_write, .finish = b_finish, .abort = b_abort };
    ota_stream_init(&s, &backend);
    s_len = 0;
    s_aborts = s_begins = 0;
    s_digest_ok = true;
    s_fail_write = false;
}

static app_err_t begin(int32_t id, uint32_t size, uint8_t origin, uint32_t now)
{
    app_cmd_t c = { .type = APP_CMD_OTA_BEGIN, .id = id, .has_id = true, .ota_size = size, .origin = origin };
    const char *detail;
    return ota_stream_command(&s, &c, now, &detail);
}

static app_err_t data(int32_t id, uint32_t at, const char *bytes, uint32_t now, const char **detail)
{
    app_cmd_t c = { .type = APP_CMD_OTA_DATA, .id = id, .has_id = true, .ota_at = at, .origin = 0 };
    c.ndef_len = (uint16_t)strlen(bytes);
    memcpy(c.ndef, bytes, c.ndef_len);
    return ota_stream_command(&s, &c, now, detail);
}

static app_err_t finish(int32_t id, uint32_t now, const char **detail)
{
    app_cmd_t c = { .type = APP_CMD_OTA_END, .id = id, .has_id = true, .origin = 0 };
    return ota_stream_command(&s, &c, now, detail);
}

static void test_a_whole_image_in_order(void)
{
    start();
    const char *detail;
    TEST_ASSERT_EQUAL(APP_ERR_NONE, begin(5, 10, 0, 0));
    TEST_ASSERT_TRUE(ota_stream_active(&s));
    TEST_ASSERT_EQUAL(APP_ERR_NONE, data(5, 0, "01234", 10, &detail));
    TEST_ASSERT_EQUAL(APP_ERR_NONE, data(5, 5, "56789", 20, &detail));
    TEST_ASSERT_EQUAL(10, ota_stream_received(&s));
    TEST_ASSERT_EQUAL(APP_ERR_NONE, finish(5, 30, &detail));
    TEST_ASSERT_FALSE(ota_stream_active(&s));
    TEST_ASSERT_EQUAL_MEMORY("0123456789", s_written, 10);
    TEST_ASSERT_EQUAL(0, s_aborts);
}

static void test_what_ends_a_session(void)
{
    const char *detail;

    start();       /* out of order */
    begin(5, 10, 0, 0);
    data(5, 0, "01234", 1, &detail);
    TEST_ASSERT_EQUAL(APP_ERR_BAD_ARG, data(5, 4, "45678", 2, &detail));
    TEST_ASSERT_EQUAL_STRING("at: expected 5", detail);
    TEST_ASSERT_FALSE(ota_stream_active(&s));
    TEST_ASSERT_EQUAL(1, s_aborts);
    TEST_ASSERT_EQUAL(APP_ERR_NO_JOB, data(5, 5, "56789", 3, &detail));

    start();       /* more than it said */
    begin(5, 6, 0, 0);
    data(5, 0, "01234", 1, &detail);
    TEST_ASSERT_EQUAL(APP_ERR_BAD_ARG, data(5, 5, "56", 2, &detail));
    TEST_ASSERT_EQUAL(1, s_aborts);

    start();       /* ended early */
    begin(5, 10, 0, 0);
    data(5, 0, "01234", 1, &detail);
    TEST_ASSERT_EQUAL(APP_ERR_BAD_ARG, finish(5, 2, &detail));
    TEST_ASSERT_EQUAL_STRING("only 5 of 10 bytes sent", detail);
    TEST_ASSERT_EQUAL(1, s_aborts);

    start();       /* the wrong image */
    s_digest_ok = false;
    begin(5, 5, 0, 0);
    data(5, 0, "01234", 1, &detail);
    TEST_ASSERT_EQUAL(APP_ERR_VERIFY_FAILED, finish(5, 2, &detail));
    TEST_ASSERT_EQUAL_STRING("sha256 does not match", detail);
    TEST_ASSERT_FALSE(ota_stream_active(&s));

    start();       /* the flash says no */
    begin(5, 5, 0, 0);
    s_fail_write = true;
    TEST_ASSERT_EQUAL(APP_ERR_WRITE_FAILED, data(5, 0, "01234", 1, &detail));
    TEST_ASSERT_FALSE(ota_stream_active(&s));

    start();       /* too big for the slot: never started */
    TEST_ASSERT_EQUAL(APP_ERR_BAD_ARG, begin(5, 1000, 0, 0));
    TEST_ASSERT_FALSE(ota_stream_active(&s));
    TEST_ASSERT_EQUAL(0, s_aborts);

    start();       /* another id */
    begin(5, 10, 0, 0);
    TEST_ASSERT_EQUAL(APP_ERR_NO_JOB, data(6, 0, "01234", 1, &detail));
    TEST_ASSERT_TRUE(ota_stream_active(&s));    /* a stray line does not end the right one */

    start();       /* the link goes */
    begin(5, 10, 0, 0);
    TEST_ASSERT_FALSE(ota_stream_link_down(&s, 1));
    TEST_ASSERT_TRUE(ota_stream_link_down(&s, 0));
    TEST_ASSERT_FALSE(ota_stream_active(&s));
    TEST_ASSERT_EQUAL(1, s_aborts);

    start();       /* nothing more arrives */
    begin(5, 10, 0, 1000);
    data(5, 0, "01234", 2000, &detail);
    TEST_ASSERT_FALSE(ota_stream_expire(&s, 2000 + OTA_STREAM_IDLE_MS - 1));
    TEST_ASSERT_TRUE(ota_stream_expire(&s, 2000 + OTA_STREAM_IDLE_MS));
    TEST_ASSERT_FALSE(ota_stream_active(&s));
}

static void test_starting_again(void)
{
    start();
    const char *detail;
    begin(5, 10, 0, 0);
    data(5, 0, "01234", 1, &detail);
    /* The same link starting over abandons what it sent. */
    TEST_ASSERT_EQUAL(APP_ERR_NONE, begin(6, 10, 0, 2));
    TEST_ASSERT_EQUAL(1, s_aborts);
    TEST_ASSERT_EQUAL(APP_ERR_NONE, data(6, 0, "abcde", 3, &detail));
    /* Another link may not take it over while it is live, but may once it has gone quiet. */
    TEST_ASSERT_EQUAL(APP_ERR_BUSY, begin(7, 10, 1, 4));
    TEST_ASSERT_TRUE(ota_stream_active(&s));
    TEST_ASSERT_EQUAL(APP_ERR_NONE, begin(7, 10, 1, 3 + OTA_STREAM_IDLE_MS));
    TEST_ASSERT_EQUAL(2, s_aborts);
}

void run_ota_stream_tests(void)
{
    RUN_TEST(test_a_whole_image_in_order);
    RUN_TEST(test_what_ends_a_session);
    RUN_TEST(test_starting_again);
}
