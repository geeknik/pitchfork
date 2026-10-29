/* See COPYING.txt for license details. */

/*
 * m1_esp_lease.h — lifecycle lease for long-running ESP32 operations.
 *
 * The SPI link mutex already serializes individual RPCs; this lease owns
 * what the mutex cannot: the lifetime of operations that keep running
 * autonomously on the ESP after their START returns (monitor, deauth,
 * beacon, spam, karma, captive portal, softAP, ESP-NOW, handshake capture,
 * WiFi association...). Until now an app that exited without calling STOP
 * left the radio running with no owner.
 *
 * Model: every successful START registers {opcode, owner-task}; the matching
 * STOP unregisters on success; when an app task goes away the manager
 * revokes its lease — m1_esp_lease_stop_op() is invoked for each registered
 * opcode (implemented in m1_esp_client.c as the typed stop dispatch), and
 * the count is reported. This is the thesis broker behavior ("cancel
 * pending operations, reset peripheral state, record the teardown result").
 *
 * Pure registry (opaque owner tokens) — host-tested in
 * m1_csrc/test/test_m1_esp_lease.c with its own stop-op hook.
 */

#ifndef M1_ESP_LEASE_H
#define M1_ESP_LEASE_H

#include <stdint.h>
#include <stdbool.h>

/* Registered by m1_esp_client.c: perform the STOP RPC for `op`. */
void m1_esp_lease_stop_op(uint16_t op);

bool m1_esp_lease_started_by(uint16_t op, void *owner);
bool m1_esp_lease_stopped_by(uint16_t op, void *owner);

/* Device convenience wrappers (owner = current task). */
void m1_esp_lease_track(uint16_t op);
void m1_esp_lease_untrack(uint16_t op);

/* Tear down everything owned by `owner`: each entry is removed first, then
 * stopped via m1_esp_lease_stop_op (a stop that re-enters untrack finds
 * nothing). Returns the number of ops stopped. */
int m1_esp_lease_revoke(void *owner);

int  m1_esp_lease_count(void);
bool m1_esp_lease_has(uint16_t op);

#endif /* M1_ESP_LEASE_H */
