#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "proto.h"
#include "test_util.h"

static app_cmd_t s_cmd;
static proto_err_t s_err;
static char s_line[PROTO_LINE_MAX];

static bool parse(const char *line)
{
    return proto_parse(line, strlen(line), &s_cmd, &s_err);
}

static void assert_rejected(const char *line, app_err_t error, const char *cmd)
{
    TEST_ASSERT_FALSE_MESSAGE(parse(line), line);
    TEST_ASSERT_EQUAL_MESSAGE(error, s_err.error, line);
    if (cmd) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cmd, s_err.cmd, line);
    } else {
        TEST_ASSERT_NULL_MESSAGE(s_err.cmd, line);
    }
}

static const char *format(const app_evt_t *evt)
{
    const size_t n = proto_format(evt, s_line, sizeof(s_line));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL('\n', s_line[n - 1]);
    TEST_ASSERT_EQUAL(n, strlen(s_line));
    s_line[n - 1] = '\0';               /* compare without the terminator */
    return s_line;
}

/* {"cmd":"program","id":7,"ndef":"<hex of the InvenTree message>"<extra>} */
static const char *program_line(const char *extra)
{
    static char line[512];
    uint8_t msg[128];
    const size_t n = make_inventree_ndef(msg, sizeof(msg), "h.example/web/stock/location/42", "INV-SL42");
    size_t at = (size_t)snprintf(line, sizeof(line), "{\"cmd\":\"program\",\"id\":7,\"ndef\":\"");
    for (size_t i = 0; i < n; i++) {
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%02x", msg[i]);
    }
    snprintf(line + at, sizeof(line) - at, "\"%s}", extra);
    return line;
}

static void test_parse_simple_commands(void)
{
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"info\"}"));
    TEST_ASSERT_EQUAL(APP_CMD_INFO, s_cmd.type);
    TEST_ASSERT_TRUE(parse("  {\"cmd\" : \"bootloader\"}\r"));
    TEST_ASSERT_EQUAL(APP_CMD_BOOTLOADER, s_cmd.type);

    TEST_ASSERT_TRUE(parse("{\"cmd\":\"cancel\"}"));
    TEST_ASSERT_EQUAL(APP_CMD_CANCEL, s_cmd.type);
    TEST_ASSERT_FALSE(s_cmd.has_id);
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"cancel\",\"id\":9}"));
    TEST_ASSERT_TRUE(s_cmd.has_id);
    TEST_ASSERT_EQUAL(9, s_cmd.id);

    TEST_ASSERT_TRUE(parse("{\"cmd\":\"hid\",\"enabled\":false}"));
    TEST_ASSERT_EQUAL(APP_CMD_HID, s_cmd.type);
    TEST_ASSERT_FALSE(s_cmd.enabled);
    TEST_ASSERT_FALSE(s_cmd.persist);
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"hid\",\"enabled\":true,\"persist\":true}"));
    TEST_ASSERT_TRUE(s_cmd.enabled);
    TEST_ASSERT_TRUE(s_cmd.persist);

    TEST_ASSERT_TRUE(parse("{\"cmd\":\"log\",\"level\":\"debug\"}"));
    TEST_ASSERT_EQUAL(APP_LOG_DEBUG, s_cmd.level);
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"log\",\"level\":\"off\"}"));
    TEST_ASSERT_EQUAL(APP_LOG_OFF, s_cmd.level);

    TEST_ASSERT_TRUE(parse("{\"cmd\":\"wipe\",\"id\":3,\"pwd\":\"a1b2c3d4\"}"));
    TEST_ASSERT_EQUAL(APP_CMD_WIPE, s_cmd.type);
    TEST_ASSERT_TRUE(s_cmd.has_pwd);
}

static void test_parse_program(void)
{
    TEST_ASSERT_TRUE(parse(program_line("")));
    TEST_ASSERT_EQUAL(APP_CMD_PROGRAM, s_cmd.type);
    TEST_ASSERT_EQUAL(7, s_cmd.id);
    TEST_ASSERT_FALSE(s_cmd.overwrite);
    TEST_ASSERT_FALSE(s_cmd.has_pwd);
    TEST_ASSERT_FALSE(s_cmd.has_old_pwd);
    TEST_ASSERT_EQUAL(APP_JOB_TIMEOUT_DEFAULT_MS, s_cmd.timeout_ms);
    uint8_t msg[128];
    const size_t n = make_inventree_ndef(msg, sizeof(msg), "h.example/web/stock/location/42", "INV-SL42");
    TEST_ASSERT_EQUAL(n, s_cmd.ndef_len);
    TEST_ASSERT_EQUAL_MEMORY(msg, s_cmd.ndef, n);

    TEST_ASSERT_TRUE(parse(program_line(",\"pwd\":\"A1B2C3D4\",\"pack\":\"1234\",\"old_pwd\":\"01020304\","
                                        "\"overwrite\":true,\"timeout_ms\":5000,\"future_field\":[1,2]")));
    TEST_ASSERT_TRUE(s_cmd.overwrite);
    TEST_ASSERT_TRUE(s_cmd.has_pwd);
    TEST_ASSERT_EQUAL_MEMORY(((const uint8_t[]){ 0xA1, 0xB2, 0xC3, 0xD4 }), s_cmd.pwd, 4);
    TEST_ASSERT_EQUAL_MEMORY(((const uint8_t[]){ 0x12, 0x34 }), s_cmd.pack, 2);
    TEST_ASSERT_TRUE(s_cmd.has_old_pwd);
    TEST_ASSERT_EQUAL_MEMORY(((const uint8_t[]){ 1, 2, 3, 4 }), s_cmd.old_pwd, 4);
    TEST_ASSERT_EQUAL(5000, s_cmd.timeout_ms);
}

static void test_parse_rejects_malformed_lines(void)
{
    assert_rejected("", APP_ERR_BAD_JSON, NULL);
    assert_rejected("hello", APP_ERR_BAD_JSON, NULL);
    assert_rejected("{\"cmd\":\"info\"", APP_ERR_BAD_JSON, NULL);
    assert_rejected("[1,2]", APP_ERR_BAD_JSON, NULL);
    assert_rejected("{}", APP_ERR_BAD_JSON, NULL);
    assert_rejected("{\"cmd\":7}", APP_ERR_BAD_JSON, NULL);
    assert_rejected("{\"cmd\":\"reboot\"}", APP_ERR_UNKNOWN_CMD, "reboot");
    assert_rejected("{\"cmd\":\"INFO\"}", APP_ERR_UNKNOWN_CMD, "INFO");
}

static void test_parse_rejects_bad_arguments(void)
{
    assert_rejected("{\"cmd\":\"program\"}", APP_ERR_BAD_ARG, "program");
    TEST_ASSERT_FALSE(s_err.has_id);
    assert_rejected("{\"cmd\":\"program\",\"id\":7}", APP_ERR_BAD_ARG, "program");
    TEST_ASSERT_TRUE(s_err.has_id);             /* enough was read to answer with the id */
    TEST_ASSERT_EQUAL(7, s_err.id);
    TEST_ASSERT_NOT_NULL(strstr(s_err.detail, "ndef"));

    assert_rejected("{\"cmd\":\"program\",\"id\":-1,\"ndef\":\"D1\"}", APP_ERR_BAD_ARG, "program");
    assert_rejected("{\"cmd\":\"program\",\"id\":1.5,\"ndef\":\"D1\"}", APP_ERR_BAD_ARG, "program");
    assert_rejected("{\"cmd\":\"program\",\"id\":\"7\",\"ndef\":\"D1\"}", APP_ERR_BAD_ARG, "program");
    assert_rejected("{\"cmd\":\"program\",\"id\":3000000000,\"ndef\":\"D1\"}", APP_ERR_BAD_ARG, "program");

    assert_rejected("{\"cmd\":\"program\",\"id\":7,\"ndef\":\"D10\"}", APP_ERR_BAD_ARG, "program");   /* odd */
    assert_rejected("{\"cmd\":\"program\",\"id\":7,\"ndef\":\"ZZ\"}", APP_ERR_BAD_ARG, "program");
    assert_rejected("{\"cmd\":\"program\",\"id\":7,\"ndef\":\"\"}", APP_ERR_BAD_ARG, "program");
    assert_rejected("{\"cmd\":\"program\",\"id\":7,\"ndef\":\"D1010B\"}", APP_ERR_BAD_ARG, "program"); /* cut short */
    assert_rejected("{\"cmd\":\"program\",\"id\":7,\"ndef\":42}", APP_ERR_BAD_ARG, "program");

    assert_rejected(program_line(",\"pwd\":\"A1B2C3\""), APP_ERR_BAD_ARG, "program");
    TEST_ASSERT_NOT_NULL(strstr(s_err.detail, "pwd"));
    assert_rejected(program_line(",\"pwd\":\"A1B2C3D4E5\""), APP_ERR_BAD_ARG, "program");
    assert_rejected(program_line(",\"pack\":\"1234\""), APP_ERR_BAD_ARG, "program");      /* pack without pwd */
    assert_rejected(program_line(",\"pwd\":\"A1B2C3D4\",\"pack\":\"12\""), APP_ERR_BAD_ARG, "program");
    assert_rejected(program_line(",\"overwrite\":1"), APP_ERR_BAD_ARG, "program");
    assert_rejected(program_line(",\"timeout_ms\":999"), APP_ERR_BAD_ARG, "program");
    assert_rejected(program_line(",\"timeout_ms\":600001"), APP_ERR_BAD_ARG, "program");
    assert_rejected(program_line(",\"timeout_ms\":\"60000\""), APP_ERR_BAD_ARG, "program");

    assert_rejected("{\"cmd\":\"wipe\"}", APP_ERR_BAD_ARG, "wipe");
    assert_rejected("{\"cmd\":\"hid\"}", APP_ERR_BAD_ARG, "hid");
    assert_rejected("{\"cmd\":\"hid\",\"enabled\":\"yes\"}", APP_ERR_BAD_ARG, "hid");
    assert_rejected("{\"cmd\":\"log\"}", APP_ERR_BAD_ARG, "log");
    assert_rejected("{\"cmd\":\"log\",\"level\":\"loud\"}", APP_ERR_BAD_ARG, "log");
    assert_rejected("{\"cmd\":\"cancel\",\"id\":\"x\"}", APP_ERR_BAD_ARG, "cancel");
    assert_rejected("{\"cmd\":\"debug\"}", APP_ERR_BAD_ARG, "debug");
    assert_rejected("{\"cmd\":\"debug\",\"action\":\"explode\"}", APP_ERR_BAD_ARG, "debug");
}

static void test_parse_echoes_an_id_on_every_command(void)
{
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"info\",\"id\":5}"));
    TEST_ASSERT_TRUE(s_cmd.has_id);
    TEST_ASSERT_EQUAL(5, s_cmd.id);
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"net\",\"id\":6}"));
    TEST_ASSERT_EQUAL(6, s_cmd.id);
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"bootloader\",\"id\":7}"));
    TEST_ASSERT_EQUAL(7, s_cmd.id);
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"info\"}"));
    TEST_ASSERT_FALSE(s_cmd.has_id);
    assert_rejected("{\"cmd\":\"info\",\"id\":\"x\"}", APP_ERR_BAD_ARG, "info");

    /* An unknown command is answered by its name, with its id, so the sender can match it. */
    assert_rejected("{\"cmd\":\"fly\",\"id\":9}", APP_ERR_UNKNOWN_CMD, "fly");
    TEST_ASSERT_TRUE(s_err.has_id);
    TEST_ASSERT_EQUAL(9, s_err.id);
    app_evt_t evt;
    proto_err_event(&s_err, &evt);
    TEST_ASSERT_EQUAL_STRING("{\"rsp\":\"fly\",\"ok\":false,\"id\":9,\"error\":\"unknown_cmd\"}", format(&evt));

    /* The parser recurses; nesting is capped well within a task's stack. */
    assert_rejected("{\"cmd\":\"info\",\"x\":[[[[[[[[[[[[1]]]]]]]]]]]]}", APP_ERR_BAD_JSON, NULL);
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"info\",\"x\":[[1]]}"));
}

static void test_parse_net_and_ota(void)
{
    TEST_ASSERT_TRUE(parse("{\"cmd\":\"net\"}"));
    TEST_ASSERT_EQUAL(APP_CMD_NET, s_cmd.type);
    TEST_ASSERT_EQUAL(APP_NET_STATUS, s_cmd.net_action);
    TEST_ASSERT_FALSE(s_cmd.has_enabled);

    TEST_ASSERT_TRUE(parse("{\"cmd\":\"net\",\"enabled\":false}"));
    TEST_ASSERT_EQUAL(APP_NET_STATUS, s_cmd.net_action);
    TEST_ASSERT_TRUE(s_cmd.has_enabled);
    TEST_ASSERT_FALSE(s_cmd.enabled);

    TEST_ASSERT_TRUE(parse("{\"cmd\":\"net\",\"action\":\"join\",\"ssid\":\"workshop\"}"));
    TEST_ASSERT_EQUAL(APP_NET_JOIN, s_cmd.net_action);
    TEST_ASSERT_EQUAL_STRING("", s_cmd.psk);           /* an open network */

    TEST_ASSERT_TRUE(parse("{\"cmd\":\"net\",\"action\":\"server\",\"url\":\"https://h.example/plugin/nfcscanner\",\"token\":\"inv-abc\"}"));
    TEST_ASSERT_EQUAL(APP_NET_SERVER, s_cmd.net_action);
    TEST_ASSERT_EQUAL_STRING("https://h.example/plugin/nfcscanner", s_cmd.url);
    TEST_ASSERT_TRUE(s_cmd.has_token);
    TEST_ASSERT_EQUAL_STRING("inv-abc", s_cmd.token);

    TEST_ASSERT_TRUE(parse("{\"cmd\":\"net\",\"action\":\"poll\",\"wait_s\":0}"));
    TEST_ASSERT_EQUAL(APP_NET_POLL, s_cmd.net_action);
    TEST_ASSERT_TRUE(s_cmd.has_wait_s);
    TEST_ASSERT_FALSE(s_cmd.has_poll_ms);
    TEST_ASSERT_EQUAL(0, s_cmd.wait_s);

    assert_rejected("{\"cmd\":\"net\",\"action\":\"join\"}", APP_ERR_BAD_ARG, "net");
    assert_rejected("{\"cmd\":\"net\",\"action\":\"join\",\"ssid\":\"x\",\"psk\":\"short\"}", APP_ERR_BAD_ARG, "net");
    assert_rejected("{\"cmd\":\"net\",\"action\":\"server\",\"url\":\"ftp://h\"}", APP_ERR_BAD_ARG, "net");
    assert_rejected("{\"cmd\":\"net\",\"action\":\"poll\"}", APP_ERR_BAD_ARG, "net");
    assert_rejected("{\"cmd\":\"net\",\"action\":\"poll\",\"poll_ms\":10}", APP_ERR_BAD_ARG, "net");
    assert_rejected("{\"cmd\":\"net\",\"action\":\"fly\"}", APP_ERR_BAD_ARG, "net");
    assert_rejected("{\"cmd\":\"net\",\"enabled\":1}", APP_ERR_BAD_ARG, "net");

    TEST_ASSERT_TRUE(parse("{\"cmd\":\"ota\",\"url\":\"https://h.example/fw.bin\",\"sha256\":\""
                           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}"));
    TEST_ASSERT_EQUAL(APP_CMD_OTA, s_cmd.type);
    TEST_ASSERT_TRUE(s_cmd.has_sha256);
    TEST_ASSERT_EQUAL_HEX8(0x01, s_cmd.sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, s_cmd.sha256[31]);
    assert_rejected("{\"cmd\":\"ota\",\"url\":\"http://10.0.0.2:8000/fw.bin\"}", APP_ERR_BAD_ARG, "ota");  /* no digest */
    assert_rejected("{\"cmd\":\"ota\"}", APP_ERR_BAD_ARG, "ota");
    assert_rejected("{\"cmd\":\"ota\",\"url\":\"https://h/fw\",\"sha256\":\"abc\"}", APP_ERR_BAD_ARG, "ota");
}

static void test_format_net_and_ota_events(void)
{
    const app_net_status_t net = {
        .enabled = true, .wifi = "connecting", .ssid = "workshop", .reader = "nfc-34b7da52a084",
        .link = "off", .poll_ms = 1000, .wait_s = 25,
    };
    app_evt_t evt = { .type = APP_EVT_NET, .net = &net };
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"net\",\"enabled\":true,\"wifi\":\"connecting\",\"ssid\":\"workshop\","
                             "\"ip\":null,\"url\":null,\"token\":false,\"reader\":\"nfc-34b7da52a084\",\"link\":\"off\","
                             "\"last_status\":0,\"poll_ms\":1000,\"wait_s\":25,\"queued\":0,\"dropped\":0}", format(&evt));

    app_evt_t ota = { .type = APP_EVT_OTA, .state = "failed", .error = APP_ERR_NFC_ERROR, .detail = "image too large" };
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"ota\",\"state\":\"failed\",\"error\":\"nfc_error\",\"detail\":\"image too large\"}", format(&ota));

    /* info carries the network side too, or says there is none */
    app_sysinfo_t sys = { .fw = "0.2.0", .idf = "v6.1", .reset = "poweron", .net = &net };
    app_evt_t info = { .type = APP_EVT_RSP, .cmd = "info", .ok = true, .sys = &sys, .state = "idle", .has_hid = true };
    TEST_ASSERT_NOT_NULL(strstr(format(&info), ",\"uptime_ms\":0,\"net\":{\"enabled\":true,\"wifi\":\"connecting\""));
    sys.net = NULL;
    TEST_ASSERT_NOT_NULL(strstr(format(&info), ",\"uptime_ms\":0,\"net\":null}"));
}

static void test_parse_accepts_the_largest_message_a_line_can_carry(void)
{
    /* NTAG216 holds 867 bytes of message; in hex, with the rest of the command, that must
     * still fit a line. */
    static char line[PROTO_LINE_MAX];
    static uint8_t msg[900];
    const size_t n = make_text_ndef(msg, sizeof(msg), 867 - 10);
    TEST_ASSERT_EQUAL(867, n);
    size_t at = (size_t)snprintf(line, sizeof(line), "{\"cmd\":\"program\",\"id\":2147483647,\"ndef\":\"");
    for (size_t i = 0; i < n; i++) {
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%02X", msg[i]);
    }
    at += (size_t)snprintf(line + at, sizeof(line) - at,
                           "\",\"pwd\":\"A1B2C3D4\",\"pack\":\"1234\",\"old_pwd\":\"01020304\","
                           "\"overwrite\":true,\"timeout_ms\":600000}");
    TEST_ASSERT_TRUE(at + 1 < PROTO_LINE_MAX);
    TEST_ASSERT_TRUE(parse(line));
    TEST_ASSERT_EQUAL(867, s_cmd.ndef_len);
    TEST_ASSERT_EQUAL(2147483647, s_cmd.id);
}

static void test_parse_errors_become_lines(void)
{
    app_evt_t evt;

    TEST_ASSERT_FALSE(parse("{\"cmd\":\"program\",\"id\":7}"));
    proto_err_event(&s_err, &evt);
    TEST_ASSERT_EQUAL_STRING(
        "{\"rsp\":\"program\",\"ok\":false,\"id\":7,\"error\":\"bad_arg\",\"detail\":\"ndef: required, as hex\"}",
        format(&evt));

    TEST_ASSERT_FALSE(parse("not json"));
    proto_err_event(&s_err, &evt);
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"error\",\"error\":\"bad_json\",\"detail\":\"expected one JSON object\"}",
                             format(&evt));

    TEST_ASSERT_FALSE(parse("{\"cmd\":\"nope\"}"));
    proto_err_event(&s_err, &evt);
    TEST_ASSERT_EQUAL_STRING("{\"rsp\":\"nope\",\"ok\":false,\"error\":\"unknown_cmd\"}", format(&evt));
}

static void test_format_job_events(void)
{
    const uint8_t uid[7] = { 0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };
    app_evt_t evt = { .type = APP_EVT_WAITING, .has_id = true, .id = 7, .timeout_ms = 60000 };
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"waiting\",\"id\":7,\"timeout_ms\":60000}", format(&evt));

    evt = (app_evt_t){ .type = APP_EVT_WRITING, .has_id = true, .id = 7, .uid_len = 7 };
    memcpy(evt.uid, uid, 7);
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"writing\",\"id\":7,\"uid\":\"04A1B2C3D4E5F6\"}", format(&evt));

    evt.type = APP_EVT_DONE;
    evt.tag_type = "ntag215";
    evt.has_protected = true;
    evt.is_protected = true;
    TEST_ASSERT_EQUAL_STRING(
        "{\"evt\":\"done\",\"id\":7,\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\",\"protected\":true}", format(&evt));

    evt = (app_evt_t){ .type = APP_EVT_FAILED, .has_id = true, .id = 7, .error = APP_ERR_TAG_REMOVED, .uid_len = 7 };
    memcpy(evt.uid, uid, 7);
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"failed\",\"id\":7,\"error\":\"tag_removed\",\"uid\":\"04A1B2C3D4E5F6\"}",
                             format(&evt));

    evt = (app_evt_t){ .type = APP_EVT_FAILED, .has_id = true, .id = 8, .error = APP_ERR_TIMEOUT };
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"failed\",\"id\":8,\"error\":\"timeout\"}", format(&evt));
}

static void test_format_tag_events(void)
{
    const uint8_t uid[7] = { 0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };
    app_evt_t evt = {
        .type = APP_EVT_TAG, .uid_len = 7, .tag_type = "ntag215",
        .text = "INV-SL42", .uri = "https://h.example/web/stock/location/42",
        .has_protected = true, .is_protected = false,
    };
    memcpy(evt.uid, uid, 7);
    TEST_ASSERT_EQUAL_STRING(
        "{\"evt\":\"tag\",\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\",\"text\":\"INV-SL42\","
        "\"uri\":\"https://h.example/web/stock/location/42\",\"protected\":false}", format(&evt));

    evt = (app_evt_t){ .type = APP_EVT_TAG, .error = APP_ERR_MULTIPLE_TAGS };
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"tag\",\"error\":\"multiple_tags\"}", format(&evt));

    evt = (app_evt_t){ .type = APP_EVT_TAG_REMOVED, .uid_len = 4 };
    memcpy(evt.uid, uid, 4);
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"tag_removed\",\"uid\":\"04A1B2C3\"}", format(&evt));
}

static void test_format_responses(void)
{
    app_sysinfo_t sys = {
        .fw = "0.1.0", .idf = "v6.1", .pn532_ok = true, .pn532_ic = 0x32, .pn532_ver = 1, .pn532_rev = 6,
        .buzzer = false, .reset = "poweron", .crash = NULL, .uptime_ms = 1234,
    };
    app_evt_t evt = {
        .type = APP_EVT_RSP, .cmd = "info", .ok = true, .sys = &sys, .state = "idle",
        .has_hid = true, .hid = true,
    };
    TEST_ASSERT_EQUAL_STRING(
        "{\"rsp\":\"info\",\"ok\":true,\"proto\":1,\"fw\":\"0.1.0\",\"idf\":\"v6.1\","
        "\"pn532\":{\"ic\":50,\"ver\":\"1.6\"},\"state\":\"idle\",\"job\":null,\"tag\":null,\"hid\":true,"
        "\"buzzer\":false,\"reset\":\"poweron\",\"crash\":null,\"uptime_ms\":1234,\"net\":null}", format(&evt));

    sys.pn532_ok = false;
    sys.crash = "panic in \"nfc_app\"";
    evt.state = "nfc_error";
    evt.has_job = true;
    evt.job_id = 7;
    evt.uid_len = 7;
    memcpy(evt.uid, ((const uint8_t[]){ 0x04, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 }), 7);
    TEST_ASSERT_EQUAL_STRING(
        "{\"rsp\":\"info\",\"ok\":true,\"proto\":1,\"fw\":\"0.1.0\",\"idf\":\"v6.1\","
        "\"pn532\":null,\"state\":\"nfc_error\",\"job\":7,\"tag\":\"04A1B2C3D4E5F6\",\"hid\":true,"
        "\"buzzer\":false,\"reset\":\"poweron\",\"crash\":\"panic in \\\"nfc_app\\\"\",\"uptime_ms\":1234,\"net\":null}",
        format(&evt));

    evt = (app_evt_t){ .type = APP_EVT_RSP, .cmd = "program", .ok = false, .error = APP_ERR_BUSY, .has_id = true, .id = 8 };
    TEST_ASSERT_EQUAL_STRING("{\"rsp\":\"program\",\"ok\":false,\"id\":8,\"error\":\"busy\"}", format(&evt));

    evt = (app_evt_t){ .type = APP_EVT_RSP, .cmd = "hid", .ok = true, .has_hid = true, .hid = false };
    TEST_ASSERT_EQUAL_STRING("{\"rsp\":\"hid\",\"ok\":true,\"enabled\":false}", format(&evt));

    evt = (app_evt_t){ .type = APP_EVT_HELLO, .sys = &sys };
    TEST_ASSERT_EQUAL_STRING("{\"evt\":\"hello\",\"proto\":1,\"fw\":\"0.1.0\"}", format(&evt));
}

static void test_format_log_stays_one_line(void)
{
    /* Whatever a log message contains, the stream stays one JSON object per line. */
    app_evt_t evt = { .type = APP_EVT_LOG, .lvl = 'W', .log_tag = "pn532", .msg = "bad \"frame\"\r\n\tretry" };
    const char *line = format(&evt);
    TEST_ASSERT_EQUAL_STRING(
        "{\"evt\":\"log\",\"lvl\":\"W\",\"tag\":\"pn532\",\"msg\":\"bad \\\"frame\\\"\\r\\n\\tretry\"}", line);
    TEST_ASSERT_NULL(strchr(line, '\n'));
}

static void test_format_reports_what_does_not_fit(void)
{
    app_evt_t evt = { .type = APP_EVT_WAITING, .has_id = true, .id = 7, .timeout_ms = 60000 };
    char small[16];
    TEST_ASSERT_EQUAL(0, proto_format(&evt, small, sizeof(small)));
}

void run_proto_tests(void)
{
    RUN_TEST(test_parse_simple_commands);
    RUN_TEST(test_parse_program);
    RUN_TEST(test_parse_rejects_malformed_lines);
    RUN_TEST(test_parse_rejects_bad_arguments);
    RUN_TEST(test_parse_echoes_an_id_on_every_command);
    RUN_TEST(test_parse_net_and_ota);
    RUN_TEST(test_format_net_and_ota_events);
    RUN_TEST(test_parse_accepts_the_largest_message_a_line_can_carry);
    RUN_TEST(test_parse_errors_become_lines);
    RUN_TEST(test_format_job_events);
    RUN_TEST(test_format_tag_events);
    RUN_TEST(test_format_responses);
    RUN_TEST(test_format_log_stays_one_line);
    RUN_TEST(test_format_reports_what_does_not_fit);
}
