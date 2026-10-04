/*
 * The state machine, driven the way the firmware drives it: command lines in through proto,
 * a simulated tag presented and taken away, time moved by hand. What it says is checked as the
 * exact lines a host would receive.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "app_core.h"
#include "proto.h"
#include "sim_ntag.h"
#include "test_util.h"

#define MAX_LINES 16

static app_core_t s_core;
static sim_ntag_t s_sim;
static app_cmd_t s_cmd;

static char s_lines[MAX_LINES][400];
static int s_nlines;
static uint32_t s_now;
static char s_typed[4][NDEF_TEXT_MAX + 1];
static int s_ntyped;
static app_feedback_t s_fb[32];
static int s_nfb;
static int s_bootloader_calls;
static int s_saved_hid;             /* -1: never saved */
static int s_log_level;

static const app_tag_t TAG = {
    .uid_len = 7, .uid = { 0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 }, .sak = 0x00, .atqa = 0x0044,
};
static const app_tag_t CLASSIC = {
    .uid_len = 4, .uid = { 0xDE, 0xAD, 0xBE, 0xEF }, .sak = 0x08, .atqa = 0x0004,
};

#define URI_REST "inventree.example/web/stock/location/42"
#define PROGRAM  "{\"cmd\":\"program\",\"id\":7,\"ndef\":\"%s\"%s}"

static void env_emit(void *ctx, const app_evt_t *evt)
{
    (void)ctx;
    TEST_ASSERT_TRUE_MESSAGE(s_nlines < MAX_LINES, "more events than the test expected");
    const size_t n = proto_format(evt, s_lines[s_nlines], sizeof(s_lines[0]));
    TEST_ASSERT_TRUE(n > 0);
    s_lines[s_nlines][n - 1] = '\0';
    s_nlines++;
}

static uint32_t env_now(void *ctx)
{
    (void)ctx;
    return s_now;
}

static void env_sysinfo(void *ctx, app_sysinfo_t *out)
{
    (void)ctx;
    out->fw = "0.1.0";
    out->idf = "v6.1";
    out->pn532_ok = s_core.nfc_ok;
    out->pn532_ic = 0x32;
    out->pn532_ver = 1;
    out->pn532_rev = 6;
    out->reset = "poweron";
    out->uptime_ms = s_now;
}

static void env_feedback(void *ctx, app_feedback_t fb)
{
    (void)ctx;
    if (s_nfb < (int)(sizeof(s_fb) / sizeof(s_fb[0]))) {
        s_fb[s_nfb++] = fb;
    }
}

static void env_hid_type(void *ctx, const char *text)
{
    (void)ctx;
    TEST_ASSERT_TRUE(s_ntyped < 4);
    snprintf(s_typed[s_ntyped++], sizeof(s_typed[0]), "%s", text);
}

static void env_hid_save(void *ctx, bool enabled)
{
    (void)ctx;
    s_saved_hid = enabled;
}

static void env_log_level(void *ctx, app_log_level_t level)
{
    (void)ctx;
    s_log_level = level;
}

static void env_bootloader(void *ctx)
{
    (void)ctx;
    s_bootloader_calls++;
}

static void forget(void)
{
    s_nlines = 0;
    s_ntyped = 0;
    s_nfb = 0;
}

/* A device that has started, with a working reader and a factory NTAG215 nearby. */
static void start(void)
{
    const app_env_t env = {
        .emit = env_emit,
        .now_ms = env_now,
        .sysinfo = env_sysinfo,
        .feedback = env_feedback,
        .hid_type = env_hid_type,
        .hid_save_default = env_hid_save,
        .set_log_level = env_log_level,
        .enter_bootloader = env_bootloader,
    };
    s_now = 1000;
    s_bootloader_calls = 0;
    s_saved_hid = -1;
    s_log_level = -1;
    app_core_init(&s_core, &env, true);
    app_core_nfc_state(&s_core, true);
    sim_ntag_init(&s_sim, NTAG_215);
    forget();
}

static void cmd(const char *line)
{
    proto_err_t err;
    TEST_ASSERT_TRUE_MESSAGE(proto_parse(line, strlen(line), &s_cmd, &err), line);
    app_core_command(&s_core, &s_cmd);
}

static const char *ndef_hex(const char *text)
{
    static char hex[400];
    uint8_t msg[160];
    const size_t n = make_inventree_ndef(msg, sizeof(msg), URI_REST, text);
    for (size_t i = 0; i < n; i++) {
        snprintf(hex + 2 * i, 3, "%02X", msg[i]);
    }
    return hex;
}

static void program(const char *extra)
{
    static char line[700];
    snprintf(line, sizeof(line), PROGRAM, ndef_hex("INV-SL42"), extra);
    cmd(line);
}

static void tap(void)
{
    sim_ntag_present(&s_sim);
    app_core_tag_arrived(&s_core, &TAG, sim_ntag_xcvr(&s_sim));
}

/* Put the InvenTree message for `text` on the simulated tag, as if written earlier. */
static void tag_holds(const char *text)
{
    uint8_t msg[160];
    const size_t n = make_inventree_ndef(msg, sizeof(msg), URI_REST, text);
    sim_ntag_set_ndef(&s_sim, msg, n);
}

#define ASSERT_LINES(n) TEST_ASSERT_EQUAL_MESSAGE((n), s_nlines, "number of lines sent")
#define ASSERT_LINE(i, want) TEST_ASSERT_EQUAL_STRING((want), s_lines[(i)])

static void test_info(void)
{
    start();
    cmd("{\"cmd\":\"info\"}");
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"rsp\":\"info\",\"ok\":true,\"proto\":1,\"fw\":\"0.1.0\",\"idf\":\"v6.1\","
                   "\"pn532\":{\"ic\":50,\"ver\":\"1.6\"},\"state\":\"idle\",\"job\":null,\"tag\":null,\"hid\":true,"
                   "\"buzzer\":false,\"reset\":\"poweron\",\"crash\":null,\"uptime_ms\":1000}");

    forget();
    program("");
    forget();
    cmd("{\"cmd\":\"info\"}");
    TEST_ASSERT_NOT_NULL(strstr(s_lines[0], "\"state\":\"waiting\",\"job\":7,\"tag\":null"));

    /* The tag on the reader is reported for as long as it is there. */
    tap();
    forget();
    cmd("{\"cmd\":\"info\"}");
    TEST_ASSERT_NOT_NULL(strstr(s_lines[0], "\"job\":null,\"tag\":\"04A1B2C3D4E5F6\""));
    app_core_tag_removed(&s_core, &TAG);
    forget();
    cmd("{\"cmd\":\"info\"}");
    TEST_ASSERT_NOT_NULL(strstr(s_lines[0], "\"tag\":null"));
}

static void test_program_blank_tag(void)
{
    start();
    program("");
    ASSERT_LINES(2);
    ASSERT_LINE(0, "{\"rsp\":\"program\",\"ok\":true,\"id\":7}");
    ASSERT_LINE(1, "{\"evt\":\"waiting\",\"id\":7,\"timeout_ms\":60000}");
    TEST_ASSERT_TRUE(app_core_wants_tag(&s_core));

    forget();
    tap();
    ASSERT_LINES(2);
    ASSERT_LINE(0, "{\"evt\":\"writing\",\"id\":7,\"uid\":\"04A1B2C3D4E5F6\"}");
    ASSERT_LINE(1, "{\"evt\":\"done\",\"id\":7,\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\",\"protected\":false}");
    TEST_ASSERT_FALSE(app_core_wants_tag(&s_core));
    TEST_ASSERT_EQUAL(0, s_ntyped);             /* a job never types */

    /* What went onto the tag is what a lookup now reports. */
    forget();
    tap();
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"evt\":\"tag\",\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\",\"text\":\"INV-SL42\","
                   "\"uri\":\"https://" URI_REST "\",\"protected\":false}");
}

static void test_program_with_password(void)
{
    start();
    program(",\"pwd\":\"A1B2C3D4\",\"pack\":\"1234\"");
    forget();
    tap();
    ASSERT_LINES(2);
    ASSERT_LINE(1, "{\"evt\":\"done\",\"id\":7,\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\",\"protected\":true}");
    TEST_ASSERT_EQUAL_HEX8(0x03, sim_ntag_page(&s_sim, 0x83)[3]);

    /* Still readable without the password, and reported as protected. */
    forget();
    tap();
    TEST_ASSERT_NOT_NULL(strstr(s_lines[0], "\"text\":\"INV-SL42\""));
    TEST_ASSERT_NOT_NULL(strstr(s_lines[0], "\"protected\":true"));

    /* Overwriting needs the password. */
    forget();
    program(",\"overwrite\":true");
    forget();
    tap();
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"auth_required\",\"uid\":\"04A1B2C3D4E5F6\"}");

    program(",\"overwrite\":true,\"pwd\":\"00000000\"");
    forget();
    tap();
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"auth_failed\",\"uid\":\"04A1B2C3D4E5F6\"}");

    program(",\"overwrite\":true,\"pwd\":\"A1B2C3D4\",\"pack\":\"1234\"");
    forget();
    tap();
    ASSERT_LINES(2);
    TEST_ASSERT_NOT_NULL(strstr(s_lines[1], "\"evt\":\"done\""));
}

static void test_program_refuses_a_written_tag(void)
{
    start();
    tag_holds("INV-SL9");
    program("");
    forget();
    tap();
    ASSERT_LINES(1);            /* no "writing": nothing was written */
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"not_blank\",\"uid\":\"04A1B2C3D4E5F6\","
                   "\"text\":\"INV-SL9\",\"uri\":\"https://" URI_REST "\"}");
    TEST_ASSERT_EQUAL(0, s_sim.writes);
    TEST_ASSERT_FALSE(app_core_wants_tag(&s_core));

    program(",\"overwrite\":true");
    forget();
    tap();
    ASSERT_LINES(2);
    TEST_ASSERT_NOT_NULL(strstr(s_lines[1], "\"evt\":\"done\""));
}

static void test_one_job_at_a_time(void)
{
    start();
    program("");
    forget();
    cmd("{\"cmd\":\"program\",\"id\":8,\"ndef\":\"D101035400656E\"}");
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"rsp\":\"program\",\"ok\":false,\"id\":8,\"error\":\"busy\"}");

    forget();
    cmd("{\"cmd\":\"wipe\",\"id\":9}");
    ASSERT_LINE(0, "{\"rsp\":\"wipe\",\"ok\":false,\"id\":9,\"error\":\"busy\"}");

    /* The first job is untouched. */
    forget();
    tap();
    TEST_ASSERT_NOT_NULL(strstr(s_lines[1], "\"evt\":\"done\",\"id\":7"));
}

static void test_cancel(void)
{
    start();
    cmd("{\"cmd\":\"cancel\"}");
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"rsp\":\"cancel\",\"ok\":false,\"error\":\"no_job\"}");

    forget();
    program("");
    forget();
    cmd("{\"cmd\":\"cancel\",\"id\":99}");
    ASSERT_LINE(0, "{\"rsp\":\"cancel\",\"ok\":false,\"id\":99,\"error\":\"no_job\"}");
    TEST_ASSERT_TRUE(app_core_wants_tag(&s_core));

    forget();
    cmd("{\"cmd\":\"cancel\"}");
    ASSERT_LINES(2);
    ASSERT_LINE(0, "{\"rsp\":\"cancel\",\"ok\":true}");
    ASSERT_LINE(1, "{\"evt\":\"failed\",\"id\":7,\"error\":\"cancelled\"}");
    TEST_ASSERT_FALSE(app_core_wants_tag(&s_core));

    /* A tag presented afterwards is looked up, not written. */
    forget();
    tap();
    TEST_ASSERT_NOT_NULL(strstr(s_lines[0], "\"evt\":\"tag\""));
    TEST_ASSERT_EQUAL(0, s_sim.writes);
}

static void test_timeout(void)
{
    start();
    program(",\"timeout_ms\":5000");
    forget();
    s_now += 4999;
    app_core_tick(&s_core);
    ASSERT_LINES(0);
    s_now += 1;
    app_core_tick(&s_core);
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"timeout\"}");
    app_core_tick(&s_core);
    ASSERT_LINES(1);

    /* The millisecond counter wrapping during a wait changes nothing. */
    start();
    s_now = 0xFFFFFF00u;
    program(",\"timeout_ms\":5000");
    forget();
    s_now += 4999;                      /* has wrapped */
    TEST_ASSERT_TRUE(s_now < 0xFFFFFF00u);
    app_core_tick(&s_core);
    ASSERT_LINES(0);
    s_now += 1;
    app_core_tick(&s_core);
    ASSERT_LINES(1);
}

static void test_job_failures(void)
{
    /* Not a Type 2 tag at all */
    start();
    program("");
    forget();
    app_core_tag_arrived(&s_core, &CLASSIC, sim_ntag_xcvr(&s_sim));
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"wrong_tag_type\",\"uid\":\"DEADBEEF\"}");
    TEST_ASSERT_EQUAL(0, s_sim.exchanges);

    /* A Type 2 tag that is not an NTAG21x */
    start();
    s_sim.supports_version = false;
    program("");
    forget();
    tap();
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"wrong_tag_type\",\"uid\":\"04A1B2C3D4E5F6\"}");

    /* Two tags in the field */
    start();
    program("");
    forget();
    app_core_tag_conflict(&s_core);
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"multiple_tags\"}");

    /* Pulled away part-way: reported, and the tag reads as empty afterwards */
    start();
    program("");
    forget();
    s_sim.tear_after_writes = 3;
    tap();
    ASSERT_LINES(2);
    ASSERT_LINE(0, "{\"evt\":\"writing\",\"id\":7,\"uid\":\"04A1B2C3D4E5F6\"}");
    ASSERT_LINE(1, "{\"evt\":\"failed\",\"id\":7,\"error\":\"tag_removed\",\"uid\":\"04A1B2C3D4E5F6\"}");
    forget();
    tap();
    ASSERT_LINE(0, "{\"evt\":\"tag\",\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\",\"protected\":false}");

    /* Locked */
    start();
    sim_ntag_page(&s_sim, 2)[3] = 0x80;
    program("");
    forget();
    tap();
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"locked\",\"uid\":\"04A1B2C3D4E5F6\"}");

    /* Too large for an NTAG213 */
    start();
    sim_ntag_init(&s_sim, NTAG_213);
    static char line[700];
    static uint8_t big[200];
    const size_t n = make_text_ndef(big, sizeof(big), 150);
    size_t at = (size_t)snprintf(line, sizeof(line), "{\"cmd\":\"program\",\"id\":7,\"ndef\":\"");
    for (size_t i = 0; i < n; i++) {
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%02X", big[i]);
    }
    snprintf(line + at, sizeof(line) - at, "\"}");
    cmd(line);
    forget();
    tap();
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"too_large\",\"uid\":\"04A1B2C3D4E5F6\"}");
}

static void test_reader_failure(void)
{
    start();
    program("");
    forget();
    app_core_nfc_state(&s_core, false);
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"nfc_error\"}");

    forget();
    program("");
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"rsp\":\"program\",\"ok\":false,\"id\":7,\"error\":\"nfc_error\"}");
    cmd("{\"cmd\":\"info\"}");
    TEST_ASSERT_NOT_NULL(strstr(s_lines[1], "\"pn532\":null,\"state\":\"nfc_error\""));

    app_core_nfc_state(&s_core, true);
    forget();
    program("");
    ASSERT_LINE(0, "{\"rsp\":\"program\",\"ok\":true,\"id\":7}");

    /* The reader failing in the middle of a job is not blamed on the tag. */
    forget();
    s_sim.reader_broken = true;
    tap();
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":7,\"error\":\"nfc_error\",\"uid\":\"04A1B2C3D4E5F6\"}");
}

static void test_lookup_types_the_text_record(void)
{
    start();
    tag_holds("INV-SL42");
    tap();
    ASSERT_LINES(1);
    TEST_ASSERT_EQUAL(1, s_ntyped);
    TEST_ASSERT_EQUAL_STRING("INV-SL42", s_typed[0]);
    TEST_ASSERT_EQUAL(0, s_sim.writes);

    /* A blank tag, and a tag of another kind, are reported and type nothing. */
    start();
    tap();
    ASSERT_LINE(0, "{\"evt\":\"tag\",\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\",\"protected\":false}");
    TEST_ASSERT_EQUAL(0, s_ntyped);

    forget();
    app_core_tag_arrived(&s_core, &CLASSIC, sim_ntag_xcvr(&s_sim));
    ASSERT_LINE(0, "{\"evt\":\"tag\",\"uid\":\"DEADBEEF\",\"type\":\"unknown\"}");
    TEST_ASSERT_EQUAL(0, s_ntyped);

    forget();
    app_core_tag_conflict(&s_core);
    ASSERT_LINE(0, "{\"evt\":\"tag\",\"error\":\"multiple_tags\"}");

    forget();
    app_core_tag_removed(&s_core, &TAG);
    ASSERT_LINE(0, "{\"evt\":\"tag_removed\",\"uid\":\"04A1B2C3D4E5F6\"}");

    /* Text that a keyboard cannot type faithfully is reported and not typed. */
    start();
    tag_holds("INV\tSL42");
    tap();
    ASSERT_LINES(1);
    TEST_ASSERT_EQUAL(0, s_ntyped);
}

static void test_lookup_of_a_tag_that_leaves_says_nothing(void)
{
    start();
    tag_holds("INV-SL42");
    s_sim.present = false;
    TEST_ASSERT_FALSE(app_core_tag_arrived(&s_core, &TAG, sim_ntag_xcvr(&s_sim)));
    ASSERT_LINES(0);
    TEST_ASSERT_EQUAL(0, s_ntyped);
}

static void test_hid_setting(void)
{
    start();
    tag_holds("INV-SL42");

    /* Off for this session */
    cmd("{\"cmd\":\"hid\",\"enabled\":false}");
    ASSERT_LINE(0, "{\"rsp\":\"hid\",\"ok\":true,\"enabled\":false}");
    TEST_ASSERT_EQUAL(-1, s_saved_hid);
    tap();
    TEST_ASSERT_EQUAL(0, s_ntyped);
    TEST_ASSERT_NOT_NULL(strstr(s_lines[1], "\"text\":\"INV-SL42\""));     /* still reported */

    /* The host goes away without turning it back on: the default returns. */
    app_core_link(&s_core, false);
    tap();
    TEST_ASSERT_EQUAL(1, s_ntyped);

    /* Off, and kept */
    forget();
    cmd("{\"cmd\":\"hid\",\"enabled\":false,\"persist\":true}");
    TEST_ASSERT_EQUAL(0, s_saved_hid);
    app_core_link(&s_core, false);
    tap();
    TEST_ASSERT_EQUAL(0, s_ntyped);

    /* A session can still turn it on over a stored "off". */
    cmd("{\"cmd\":\"hid\",\"enabled\":true}");
    tap();
    TEST_ASSERT_EQUAL(1, s_ntyped);
}

static void test_wipe(void)
{
    start();
    program(",\"pwd\":\"A1B2C3D4\"");
    tap();
    forget();

    /* Refused for want of the password, before anything is written, and so without "writing". */
    cmd("{\"cmd\":\"wipe\",\"id\":10}");
    forget();
    tap();
    ASSERT_LINES(1);
    ASSERT_LINE(0, "{\"evt\":\"failed\",\"id\":10,\"error\":\"auth_required\",\"uid\":\"04A1B2C3D4E5F6\"}");
    forget();

    cmd("{\"cmd\":\"wipe\",\"id\":11,\"pwd\":\"A1B2C3D4\"}");
    ASSERT_LINE(0, "{\"rsp\":\"wipe\",\"ok\":true,\"id\":11}");
    ASSERT_LINE(1, "{\"evt\":\"waiting\",\"id\":11,\"timeout_ms\":60000}");
    forget();
    tap();
    ASSERT_LINES(2);
    ASSERT_LINE(0, "{\"evt\":\"writing\",\"id\":11,\"uid\":\"04A1B2C3D4E5F6\"}");
    ASSERT_LINE(1, "{\"evt\":\"done\",\"id\":11,\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\",\"protected\":false}");
    TEST_ASSERT_EQUAL_HEX8(0xFF, sim_ntag_page(&s_sim, 0x83)[3]);

    forget();
    tap();
    ASSERT_LINE(0, "{\"evt\":\"tag\",\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\",\"protected\":false}");
}

static void test_bootloader_log_and_hello(void)
{
    start();
    cmd("{\"cmd\":\"log\",\"level\":\"warn\"}");
    ASSERT_LINE(0, "{\"rsp\":\"log\",\"ok\":true}");
    TEST_ASSERT_EQUAL(APP_LOG_WARN, s_log_level);

    forget();
    app_core_link(&s_core, true);
    ASSERT_LINE(0, "{\"evt\":\"hello\",\"proto\":1,\"fw\":\"0.1.0\"}");

    /* A waiting job is ended before the device goes away. */
    forget();
    program("");
    forget();
    cmd("{\"cmd\":\"bootloader\"}");
    ASSERT_LINES(2);
    ASSERT_LINE(0, "{\"rsp\":\"bootloader\",\"ok\":true}");
    ASSERT_LINE(1, "{\"evt\":\"failed\",\"id\":7,\"error\":\"cancelled\"}");
    TEST_ASSERT_EQUAL(1, s_bootloader_calls);
    TEST_ASSERT_EQUAL(APP_FB_BOOTLOADER, s_fb[s_nfb - 1]);
}

static app_debug_action_t s_debug_action;
static int s_debug_calls;

static void env_debug(void *ctx, app_debug_action_t action)
{
    (void)ctx;
    s_debug_action = action;
    s_debug_calls++;
}

static void test_debug_exists_only_where_it_is_wired(void)
{
    start();
    cmd("{\"cmd\":\"debug\",\"action\":\"crash\"}");
    ASSERT_LINE(0, "{\"rsp\":\"debug\",\"ok\":false,\"error\":\"unknown_cmd\"}");

    s_core.env.debug = env_debug;
    s_debug_calls = 0;
    forget();
    cmd("{\"cmd\":\"debug\",\"action\":\"nousb\"}");
    ASSERT_LINE(0, "{\"rsp\":\"debug\",\"ok\":true}");
    TEST_ASSERT_EQUAL(1, s_debug_calls);
    TEST_ASSERT_EQUAL(APP_DEBUG_NOUSB, s_debug_action);
}

static void test_feedback_follows_the_state(void)
{
    start();
    program("");
    TEST_ASSERT_EQUAL(APP_FB_JOB_WAITING, s_fb[s_nfb - 1]);
    forget();
    tap();
    const app_feedback_t want[] = { APP_FB_JOB_WRITING, APP_FB_JOB_DONE, APP_FB_IDLE };
    TEST_ASSERT_EQUAL(3, s_nfb);
    TEST_ASSERT_EQUAL_INT_ARRAY(want, s_fb, 3);

    forget();
    app_core_nfc_state(&s_core, false);
    TEST_ASSERT_EQUAL(APP_FB_NFC_ERROR, s_fb[s_nfb - 1]);
    app_core_nfc_state(&s_core, true);
    TEST_ASSERT_EQUAL(APP_FB_IDLE, s_fb[s_nfb - 1]);
}

void run_app_core_tests(void)
{
    RUN_TEST(test_info);
    RUN_TEST(test_program_blank_tag);
    RUN_TEST(test_program_with_password);
    RUN_TEST(test_program_refuses_a_written_tag);
    RUN_TEST(test_one_job_at_a_time);
    RUN_TEST(test_cancel);
    RUN_TEST(test_timeout);
    RUN_TEST(test_job_failures);
    RUN_TEST(test_reader_failure);
    RUN_TEST(test_lookup_types_the_text_record);
    RUN_TEST(test_lookup_of_a_tag_that_leaves_says_nothing);
    RUN_TEST(test_hid_setting);
    RUN_TEST(test_wipe);
    RUN_TEST(test_bootloader_log_and_hello);
    RUN_TEST(test_debug_exists_only_where_it_is_wired);
    RUN_TEST(test_feedback_follows_the_state);
}
