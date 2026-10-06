/* Which network the station tries next, and when. */
#include "unity.h"

#include "wifi_policy.h"
#include "test_util.h"

static void test_policy_starts_with_the_preferred_and_moves_on(void)
{
    wifi_policy_t p;
    const bool present[WIFI_POLICY_NETWORKS_MAX] = { true, false, true, false };
    wifi_policy_init(&p, present, 2);
    TEST_ASSERT_EQUAL(2, wifi_policy_next(&p));
    TEST_ASSERT_TRUE(wifi_policy_due(&p, 1000));

    /* A failed join: the other network, after a second. */
    wifi_policy_join_failed(&p, 1000);
    TEST_ASSERT_EQUAL(0, wifi_policy_next(&p));
    TEST_ASSERT_EQUAL(1000, wifi_policy_delay_ms(&p, 1000));
    TEST_ASSERT_TRUE(wifi_policy_due(&p, 2000));
    wifi_policy_join_failed(&p, 2000);
    TEST_ASSERT_EQUAL(2, wifi_policy_next(&p));              /* round robin over those present */
    TEST_ASSERT_EQUAL(2000, wifi_policy_delay_ms(&p, 2000));

    /* Joined: no waiting, and nothing to do while it is up. */
    wifi_policy_joined(&p);
    TEST_ASSERT_EQUAL(UINT32_MAX, wifi_policy_delay_ms(&p, 4000));
    /* Lost: the same network again, after a short wait that starts over. */
    wifi_policy_link_lost(&p, 5000);
    TEST_ASSERT_EQUAL(2, wifi_policy_next(&p));
    TEST_ASSERT_EQUAL(1000, wifi_policy_delay_ms(&p, 5000));

    /* The wait grows to a ceiling. */
    for (int i = 0; i < 8; i++) {
        wifi_policy_join_failed(&p, 10000);
    }
    TEST_ASSERT_EQUAL(WIFI_POLICY_BACKOFF_MAX_MS, wifi_policy_delay_ms(&p, 10000));
}

static void test_policy_with_nothing_to_join(void)
{
    wifi_policy_t p;
    const bool none[WIFI_POLICY_NETWORKS_MAX] = { false, false, false, false };
    wifi_policy_init(&p, none, 0);
    TEST_ASSERT_EQUAL(-1, wifi_policy_next(&p));
    TEST_ASSERT_EQUAL(UINT32_MAX, wifi_policy_delay_ms(&p, 0));

    /* A preferred network that is not there: the first that is. */
    const bool some[WIFI_POLICY_NETWORKS_MAX] = { false, true, false, false };
    wifi_policy_init(&p, some, 3);
    TEST_ASSERT_EQUAL(1, wifi_policy_next(&p));
    wifi_policy_join_failed(&p, 0);
    TEST_ASSERT_EQUAL(1, wifi_policy_next(&p));             /* the only one: again */
}

void run_wifi_policy_tests(void)
{
    RUN_TEST(test_policy_starts_with_the_preferred_and_moves_on);
    RUN_TEST(test_policy_with_nothing_to_join);
}
