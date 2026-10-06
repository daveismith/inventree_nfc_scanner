/*
 * Which network to try next, and when. The radio is not in here: the station feeds it
 * what happened, and it answers with what to do, so the host tests can cover it.
 *
 * The rules: start with the preferred network. A join that fails moves on to the next
 * network that has a name, round robin. Every failure waits longer before the next try,
 * from 1 s doubling to 30 s; a success resets the wait and makes that network preferred.
 * Losing a link that was up is a failure like any other, except that the same network is
 * tried first again, since it was working a moment ago.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_POLICY_NETWORKS_MAX    4
#define WIFI_POLICY_BACKOFF_MIN_MS  1000
#define WIFI_POLICY_BACKOFF_MAX_MS  30000

typedef struct {
    bool present[WIFI_POLICY_NETWORKS_MAX];
    int current;                        /* the network being tried or used */
    uint32_t backoff_ms;                /* 0: no failure outstanding */
    uint32_t next_try_at;
    bool up;
} wifi_policy_t;

/* `preferred` is where to start; networks without a name are skipped. */
void wifi_policy_init(wifi_policy_t *p, const bool present[WIFI_POLICY_NETWORKS_MAX], int preferred);

/* -1 when there is nothing to try. */
int wifi_policy_next(const wifi_policy_t *p);

/* Whether a join may begin now. */
bool wifi_policy_due(const wifi_policy_t *p, uint32_t now_ms);
uint32_t wifi_policy_delay_ms(const wifi_policy_t *p, uint32_t now_ms);

/* What happened. */
void wifi_policy_joined(wifi_policy_t *p);
void wifi_policy_join_failed(wifi_policy_t *p, uint32_t now_ms);
void wifi_policy_link_lost(wifi_policy_t *p, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
