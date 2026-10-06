/*
 * The exchange with the plugin, driven with strings: what the reader sends, what it does
 * with what comes back, and how it paces itself when things go wrong.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "net_sync.h"
#include "test_util.h"

static net_sync_t s_ns;
static char s_body[4096];
static char s_cmds[8][NET_SYNC_CMD_MAX];
static int s_ncmds;

static void on_cmd(void *ctx, const char *json, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(s_ncmds < 8);
    TEST_ASSERT_EQUAL(len, strlen(json));
    snprintf(s_cmds[s_ncmds++], sizeof(s_cmds[0]), "%s", json);
}

static bool queue(const char *line)
{
    return net_sync_queue(&s_ns, line, strlen(line));
}

static void start(void)
{
    net_sync_init(&s_ns, "nfc-34b7da52a084", 17, 1000, 25);
    s_ncmds = 0;
}

static size_t request(uint32_t now, bool hold)
{
    const size_t n = net_sync_request(&s_ns, now, hold, s_body, sizeof(s_body));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL(n, strlen(s_body));
    return n;
}

static void answer(uint32_t now, int status, const char *body)
{
    net_sync_response(&s_ns, now, status, body, strlen(body), on_cmd, NULL);
}

static void test_request_carries_the_queue_numbered(void)
{
    start();
    request(1000, true);
    TEST_ASSERT_EQUAL_STRING("{\"reader\":\"nfc-34b7da52a084\",\"boot\":17,\"proto\":1,\"ack\":0,\"wait_s\":25,\"msgs\":[]}", s_body);

    TEST_ASSERT_TRUE(queue("{\"evt\":\"tag\",\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\"}\n"));
    TEST_ASSERT_TRUE(queue("{\"rsp\":\"program\",\"ok\":true,\"id\":317}"));
    TEST_ASSERT_TRUE(net_sync_has_pending(&s_ns));
    request(2000, false);                   /* a report: no hold asked for */
    TEST_ASSERT_EQUAL_STRING("{\"reader\":\"nfc-34b7da52a084\",\"boot\":17,\"proto\":1,\"ack\":0,\"wait_s\":0,\"msgs\":["
                             "{\"seq\":1,\"evt\":\"tag\",\"uid\":\"04A1B2C3D4E5F6\",\"type\":\"ntag215\"},"
                             "{\"seq\":2,\"rsp\":\"program\",\"ok\":true,\"id\":317}]}", s_body);

    /* Not acknowledged: sent again. Acknowledged: gone. */
    answer(2010, 200, "{\"ack\":1,\"cmds\":[]}");
    request(3000, true);
    TEST_ASSERT_NOT_NULL(strstr(s_body, "\"msgs\":[{\"seq\":2,\"rsp\":\"program\""));
    answer(3010, 200, "{\"ack\":2,\"cmds\":[]}");
    TEST_ASSERT_FALSE(net_sync_has_pending(&s_ns));
}

static void test_what_is_not_for_the_server(void)
{
    start();
    TEST_ASSERT_FALSE(queue("{\"evt\":\"hello\",\"proto\":1}"));
    TEST_ASSERT_FALSE(queue("{\"evt\":\"tag_removed\",\"uid\":\"04\"}"));
    TEST_ASSERT_FALSE(queue("{\"evt\":\"log\",\"lvl\":\"W\"}"));
    TEST_ASSERT_FALSE(queue("{\"evt\":\"net\",\"enabled\":true}"));
    TEST_ASSERT_FALSE(queue("not json"));
    TEST_ASSERT_TRUE(queue("{\"evt\":\"ota\",\"state\":\"done\"}"));
    TEST_ASSERT_TRUE(queue("{\"evt\":\"failed\",\"id\":1,\"error\":\"timeout\"}"));
    TEST_ASSERT_EQUAL(2, s_ns.count);
}

static void test_commands_are_acted_on_once_and_acknowledged(void)
{
    start();
    request(1000, true);
    answer(1010, 200, "{\"ack\":0,\"cmds\":[{\"seq\":42,\"cmd\":\"program\",\"id\":317,\"ndef\":\"D1\"},"
                      "{\"seq\":43,\"cmd\":\"cancel\",\"id\":317}],\"poll_ms\":2000}");
    TEST_ASSERT_EQUAL(2, s_ncmds);
    TEST_ASSERT_EQUAL_STRING("{\"seq\":42,\"cmd\":\"program\",\"id\":317,\"ndef\":\"D1\"}", s_cmds[0]);
    TEST_ASSERT_EQUAL_STRING("{\"seq\":43,\"cmd\":\"cancel\",\"id\":317}", s_cmds[1]);

    /* The ack goes up; a repeat of an old command is ignored; a newer one is taken. */
    request(3000, true);
    TEST_ASSERT_NOT_NULL(strstr(s_body, "\"ack\":43,"));
    answer(3010, 200, "{\"ack\":0,\"cmds\":[{\"seq\":43,\"cmd\":\"cancel\",\"id\":317},{\"seq\":44,\"cmd\":\"info\"}]}");
    TEST_ASSERT_EQUAL(3, s_ncmds);
    TEST_ASSERT_EQUAL_STRING("{\"seq\":44,\"cmd\":\"info\"}", s_cmds[2]);

    /* Rubbish in the list is skipped. */
    answer(3020, 200, "{\"ack\":0,\"cmds\":[7,{\"cmd\":\"info\"},{\"seq\":\"x\",\"cmd\":\"info\"}]}");
    TEST_ASSERT_EQUAL(3, s_ncmds);
}

static void test_pacing_plain_polling(void)
{
    start();
    net_sync_set_pacing(&s_ns, 1000, 0);
    TEST_ASSERT_TRUE(net_sync_due(&s_ns, 500));             /* the first call goes at once */
    request(500, true);
    TEST_ASSERT_NOT_NULL(strstr(s_body, "\"wait_s\":0,"));
    answer(520, 200, "{\"ack\":0,\"cmds\":[]}");
    TEST_ASSERT_EQUAL(980, net_sync_delay_ms(&s_ns, 520));  /* a second after it began */
    TEST_ASSERT_FALSE(net_sync_due(&s_ns, 1499));
    TEST_ASSERT_TRUE(net_sync_due(&s_ns, 1500));

    /* Something to report: at once. */
    request(1500, true);
    answer(1510, 200, "{\"ack\":0,\"cmds\":[]}");
    queue("{\"evt\":\"tag\",\"uid\":\"04\"}");
    TEST_ASSERT_TRUE(net_sync_due(&s_ns, 1511));

    /* The server asks for a slower pace. */
    request(1511, true);
    answer(1520, 200, "{\"ack\":1,\"cmds\":[],\"poll_ms\":5000}");
    TEST_ASSERT_EQUAL(4989, net_sync_delay_ms(&s_ns, 1522));
    answer(6600, 200, "{\"ack\":1,\"cmds\":[]}");             /* and stops asking */
    request(6600, true);
    answer(6610, 200, "{\"ack\":1,\"cmds\":[]}");
    TEST_ASSERT_EQUAL(990, net_sync_delay_ms(&s_ns, 6610));
}

static void test_pacing_failures(void)
{
    start();
    request(1000, true);
    net_sync_unreachable(&s_ns, 1100);
    TEST_ASSERT_EQUAL(1000, net_sync_delay_ms(&s_ns, 1100));
    TEST_ASSERT_EQUAL_STRING("unreachable", net_sync_link_state(&s_ns));
    request(2100, true);
    net_sync_unreachable(&s_ns, 2200);
    TEST_ASSERT_EQUAL(2000, net_sync_delay_ms(&s_ns, 2200));
    request(4200, true);
    answer(4300, 502, "<html>bad gateway</html>");
    TEST_ASSERT_EQUAL(4000, net_sync_delay_ms(&s_ns, 4300));
    TEST_ASSERT_EQUAL_STRING("error", net_sync_link_state(&s_ns));
    for (int i = 0; i < 6; i++) {
        request(9000, true);
        answer(9000, 200, "not even json");
    }
    TEST_ASSERT_EQUAL(NET_SYNC_BACKOFF_MAX_MS, net_sync_delay_ms(&s_ns, 9000));

    /* A good answer clears it; a message to report still waits out nothing. */
    request(40000, true);
    queue("{\"evt\":\"tag\",\"uid\":\"04\"}");
    answer(40010, 200, "{\"ack\":0,\"cmds\":[]}");
    TEST_ASSERT_EQUAL_STRING("ok", net_sync_link_state(&s_ns));
    TEST_ASSERT_TRUE(net_sync_due(&s_ns, 40011));

    /* Refused: stop for a long while, then try again; reconfiguring tries at once. */
    request(40011, true);
    answer(40020, 403, "{\"detail\":\"not your scanner\"}");
    TEST_ASSERT_EQUAL_STRING("refused", net_sync_link_state(&s_ns));
    TEST_ASSERT_EQUAL(NET_SYNC_REFUSED_RETRY_MS - 10, net_sync_delay_ms(&s_ns, 40030));
    TEST_ASSERT_TRUE(net_sync_has_pending(&s_ns));             /* kept for later */
    net_sync_reconfigured(&s_ns);
    TEST_ASSERT_TRUE(net_sync_due(&s_ns, 40031));
}

static void test_queue_makes_room_for_what_matters(void)
{
    start();
    for (int i = 0; i < NET_SYNC_QUEUE_LEN; i++) {
        TEST_ASSERT_TRUE(queue("{\"evt\":\"tag\",\"uid\":\"04\"}"));
    }
    TEST_ASSERT_EQUAL(NET_SYNC_QUEUE_LEN, s_ns.count);
    /* A result arriving into a full queue pushes out the oldest tap. */
    TEST_ASSERT_TRUE(queue("{\"evt\":\"done\",\"id\":5}"));
    TEST_ASSERT_EQUAL(NET_SYNC_QUEUE_LEN, s_ns.count);
    TEST_ASSERT_EQUAL(1, s_ns.dropped);
    TEST_ASSERT_EQUAL(2, s_ns.queue[s_ns.head].seq);             /* seq 1 went */
    TEST_ASSERT_EQUAL(NET_SYNC_QUEUE_LEN + 1, s_ns.queue[(s_ns.head + s_ns.count - 1) % NET_SYNC_QUEUE_LEN].seq);

    /* Taps go before results, however old the results are. */
    start();
    for (int i = 0; i < NET_SYNC_QUEUE_LEN - 1; i++) {
        queue("{\"evt\":\"done\",\"id\":5}");
    }
    queue("{\"evt\":\"tag\",\"uid\":\"04\"}");
    queue("{\"evt\":\"done\",\"id\":6}");
    for (uint8_t i = 0; i < s_ns.count; i++) {
        TEST_ASSERT_TRUE(s_ns.queue[(s_ns.head + i) % NET_SYNC_QUEUE_LEN].keep);
    }
    TEST_ASSERT_EQUAL(1, s_ns.dropped);

    /* The body reports what fits in the buffer, or nothing. */
    char small[64];
    TEST_ASSERT_EQUAL(0, net_sync_request(&s_ns, 1, true, small, sizeof(small)));
}

void run_net_sync_tests(void)
{
    RUN_TEST(test_request_carries_the_queue_numbered);
    RUN_TEST(test_what_is_not_for_the_server);
    RUN_TEST(test_commands_are_acted_on_once_and_acknowledged);
    RUN_TEST(test_pacing_plain_polling);
    RUN_TEST(test_pacing_failures);
    RUN_TEST(test_queue_makes_room_for_what_matters);
}
