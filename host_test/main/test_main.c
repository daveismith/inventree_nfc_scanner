#include <stdlib.h>

#include "unity.h"

#include "test_util.h"

void setUp(void)
{
}

void tearDown(void)
{
}

void app_main(void)
{
    UNITY_BEGIN();
    run_ndef_tests();
    run_pn532_frame_tests();
    run_ntag21x_tests();
    run_proto_tests();
    run_app_core_tests();
    run_net_sync_tests();
    run_wifi_policy_tests();
    run_ota_stream_tests();
    exit(UNITY_END() == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
