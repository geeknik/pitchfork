/* See COPYING.txt for license details. */

/*
 * m1_capture_envelope.h — the canonical M1 Capture Envelope (thesis core).
 *
 * Every capture, from every radio domain, is described by one envelope
 * record inside a session directory on SD:
 *
 *   /Sessions/<YYYYMMDD-HHMMSS>-<id4hex>/
 *       session.m1s        session manifest (created once per session)
 *       captures/          envelope records, one file per capture
 *       notes/             operator annotations (free text)
 *
 * An envelope is key=value text (same style as the crash records): stable
 * keys, additive-only (new keys may appear; readers must skip unknowns),
 * human-readable on-device and grep-able from the host.
 *
 * Keys (v1):
 *   envelope_version=1
 *   record_id, session_id            correlation anchors
 *   timestamp_monotonic_ms           device tick at capture end
 *   source_domain                    wifi|subghz|nfc|lfid|ir|ble|usb
 *   capture_parameters               domain-specific "k:v,k:v" line
 *   raw_artifact_ref                 SD path of the immutable raw capture
 *   decoder_candidates               comma list of decoder ids attempted
 *   confidence                       0..1000 (per-mille)
 *   device_entity_id                 "domain:identity" (e.g. wifi:aabbcc...)
 *   parent_record_id                 record this derived from, or '-'
 *   operator_annotation              free text, or '-'
 *   integrity_hash                   crc32c:<hex8> over the raw artifact
 *
 * Pure formatting/naming logic is host-tested (m1_csrc/test/); the SD
 * writes go through the same static-buffer discipline as m1_crash.c.
 */

#ifndef M1_CAPTURE_ENVELOPE_H
#define M1_CAPTURE_ENVELOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M1_ENV_VERSION        1
#define M1_ENV_SESSION_ID_LEN 9    /* 8 hex chars + NUL */
#define M1_ENV_DIR_MAX        48
#define M1_ENV_LINE_MAX       96

typedef struct {
    uint32_t    record_id;
    const char *session_id;         /* 8 hex chars */
    uint32_t    timestamp_monotonic_ms;
    const char *source_domain;      /* wifi|subghz|nfc|lfid|ir|ble|usb */
    const char *capture_parameters; /* "k:v,k:v" or "-" */
    const char *raw_artifact_ref;   /* SD path */
    const char *decoder_candidates; /* comma list or "-" */
    uint16_t    confidence_pm;      /* 0..1000 per-mille */
    const char *device_entity_id;   /* "domain:id" or "-" */
    int32_t     parent_record_id;   /* -1 = none */
    const char *operator_annotation;/* or "-" */
    uint32_t    integrity_crc32c;   /* over the raw artifact */
} m1_envelope_t;

/* Format one envelope record (NUL-terminated, trailing newline).
 * Returns bytes written, 0 if cap too small (retry larger). */
size_t m1_env_format(char *buf, size_t cap, const m1_envelope_t *e);

/* Session directory name: "/Sessions/<stamp>-<id>" where stamp is
 * YYYYMMDD-HHMMSS from a unix-seconds input (device RTC or host-provided).
 * Returns bytes written. id is 8 hex chars. */
size_t m1_env_session_dir(char *out, size_t cap, uint32_t unix_sec,
                          const char *id8hex);

/* Derive a stable 8-hex session id from two words of entropy (tick +
 * frames). Not cryptographic — a correlation handle, not a proof. */
void m1_env_session_id(char out[9], uint32_t a, uint32_t b);

#endif /* M1_CAPTURE_ENVELOPE_H */
