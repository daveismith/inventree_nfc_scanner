#include "wifi_policy.h"

#include <string.h>

void wifi_policy_init(wifi_policy_t *p, const bool present[WIFI_POLICY_NETWORKS_MAX], int preferred)
{
    memset(p, 0, sizeof(*p));
    memcpy(p->present, present, sizeof(p->present));
    p->current = -1;
    if (preferred >= 0 && preferred < WIFI_POLICY_NETWORKS_MAX && present[preferred]) {
        p->current = preferred;
    } else {
        for (int i = 0; i < WIFI_POLICY_NETWORKS_MAX; i++) {
            if (present[i]) {
                p->current = i;
                break;
            }
        }
    }
}

int wifi_policy_next(const wifi_policy_t *p)
{
    return p->current;
}

uint32_t wifi_policy_delay_ms(const wifi_policy_t *p, uint32_t now_ms)
{
    if (p->current < 0 || p->up) {
        return UINT32_MAX;
    }
    if (p->backoff_ms == 0) {
        return 0;
    }
    const int32_t until = (int32_t)(p->next_try_at - now_ms);
    return until > 0 ? (uint32_t)until : 0;
}

bool wifi_policy_due(const wifi_policy_t *p, uint32_t now_ms)
{
    return wifi_policy_delay_ms(p, now_ms) == 0;
}

static void wait_longer(wifi_policy_t *p, uint32_t now_ms)
{
    p->backoff_ms = p->backoff_ms == 0 ? WIFI_POLICY_BACKOFF_MIN_MS
                    : (p->backoff_ms * 2 > WIFI_POLICY_BACKOFF_MAX_MS ? WIFI_POLICY_BACKOFF_MAX_MS : p->backoff_ms * 2);
    p->next_try_at = now_ms + p->backoff_ms;
}

static void move_on(wifi_policy_t *p)
{
    for (int step = 1; step <= WIFI_POLICY_NETWORKS_MAX; step++) {
        const int i = (p->current + step) % WIFI_POLICY_NETWORKS_MAX;
        if (p->present[i]) {
            p->current = i;
            return;
        }
    }
}

void wifi_policy_joined(wifi_policy_t *p)
{
    p->up = true;
    p->backoff_ms = 0;
}

void wifi_policy_join_failed(wifi_policy_t *p, uint32_t now_ms)
{
    p->up = false;
    move_on(p);
    wait_longer(p, now_ms);
}

void wifi_policy_link_lost(wifi_policy_t *p, uint32_t now_ms)
{
    p->up = false;
    wait_longer(p, now_ms);             /* the same network first: it was working */
}
