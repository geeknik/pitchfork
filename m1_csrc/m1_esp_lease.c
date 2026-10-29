/* See COPYING.txt for license details. */

/* m1_esp_lease.c — registry for the ESP-channel operation lease.
 * See m1_esp_lease.h; this file is pure, allocation-free logic. */

#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"
#include "m1_esp_lease.h"

#define M1_ESP_LEASE_MAX_OPS 16

typedef struct {
    uint16_t op;
    void    *owner;
    bool     used;
} lease_entry_t;

static lease_entry_t s_ops[M1_ESP_LEASE_MAX_OPS];

static int find(uint16_t op)
{
    for (int i = 0; i < M1_ESP_LEASE_MAX_OPS; i++)
        if (s_ops[i].used && s_ops[i].op == op)
            return i;
    return -1;
}

bool m1_esp_lease_started_by(uint16_t op, void *owner)
{
    if (owner == 0)
        return false;
    int existing = find(op);
    if (existing >= 0) {                     /* re-start: re-own in place */
        s_ops[existing].owner = owner;
        return true;
    }
    for (int i = 0; i < M1_ESP_LEASE_MAX_OPS; i++) {
        if (!s_ops[i].used) {
            s_ops[i].op = op;
            s_ops[i].owner = owner;
            s_ops[i].used = true;
            return true;
        }
    }
    return false;                            /* table full — refuse the op */
}

bool m1_esp_lease_stopped_by(uint16_t op, void *owner)
{
    int i = find(op);
    if (i < 0 || s_ops[i].owner != owner)
        return false;                        /* not ours to stop */
    s_ops[i].used = false;
    return true;
}

void m1_esp_lease_track(uint16_t op)
{
    (void)m1_esp_lease_started_by(op, (void *)xTaskGetCurrentTaskHandle());
}

void m1_esp_lease_untrack(uint16_t op)
{
    (void)m1_esp_lease_stopped_by(op, (void *)xTaskGetCurrentTaskHandle());
}

int m1_esp_lease_revoke(void *owner)
{
    int stopped = 0;
    for (int i = 0; i < M1_ESP_LEASE_MAX_OPS; i++) {
        if (!s_ops[i].used || s_ops[i].owner != owner)
            continue;
        /* Remove the entry BEFORE dispatching the stop: a stop that calls
         * untrack re-entrantly must find nothing, and a failed stop must
         * not re-enter revoke. */
        uint16_t op = s_ops[i].op;
        s_ops[i].used = false;
        m1_esp_lease_stop_op(op);
        stopped++;
    }
    return stopped;
}

int m1_esp_lease_count(void)
{
    int n = 0;
    for (int i = 0; i < M1_ESP_LEASE_MAX_OPS; i++)
        if (s_ops[i].used)
            n++;
    return n;
}

bool m1_esp_lease_has(uint16_t op) { return find(op) >= 0; }
