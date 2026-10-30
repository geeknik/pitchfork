/* test_m1_env.c — host tests for the Capture Envelope core.
 * Run: make -C m1_csrc/test test-env
 *
 * Covers: format round-trips and truncation contract, NULL-field
 * defaults, session-dir naming across date boundaries (incl. leap year),
 * and session-id derivation stability. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "m1_capture_envelope.h"

static int g_failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0);

int main(void)
{
    char buf[1024];

    /* --- format: happy path + every key present --- */
    m1_envelope_t e;
    memset(&e, 0, sizeof(e));
    e.record_id = 7;
    e.session_id = "deadbeef";
    e.timestamp_monotonic_ms = 123456;
    e.source_domain = "wifi";
    e.capture_parameters = "chan:6,dur_ms:8000";
    e.raw_artifact_ref = "/wifi/mcap_000.pcapng";
    e.decoder_candidates = "radiotap,eapol";
    e.confidence_pm = 850;
    e.device_entity_id = "wifi:aabbccddeeff";
    e.parent_record_id = -1;
    e.operator_annotation = "first live envelope";
    e.integrity_crc32c = 0xDEADBEEF;

    size_t n = m1_env_format(buf, sizeof(buf), &e);
    CHECK(n > 0);
    CHECK(strstr(buf, "envelope_version=1\n") != NULL);
    CHECK(strstr(buf, "record_id=7\n") != NULL);
    CHECK(strstr(buf, "session_id=deadbeef\n") != NULL);
    CHECK(strstr(buf, "timestamp_monotonic_ms=123456\n") != NULL);
    CHECK(strstr(buf, "source_domain=wifi\n") != NULL);
    CHECK(strstr(buf, "capture_parameters=chan:6,dur_ms:8000\n") != NULL);
    CHECK(strstr(buf, "raw_artifact_ref=/wifi/mcap_000.pcapng\n") != NULL);
    CHECK(strstr(buf, "decoder_candidates=radiotap,eapol\n") != NULL);
    CHECK(strstr(buf, "confidence=850\n") != NULL);
    CHECK(strstr(buf, "device_entity_id=wifi:aabbccddeeff\n") != NULL);
    CHECK(strstr(buf, "parent_record_id=-1\n") != NULL);
    CHECK(strstr(buf, "operator_annotation=first live envelope\n") != NULL);
    CHECK(strstr(buf, "integrity_hash=crc32c:DEADBEEF\n") != NULL);

    /* derived child record */
    e.parent_record_id = 7;
    e.confidence_pm = 990;
    n = m1_env_format(buf, sizeof(buf), &e);
    CHECK(n > 0);
    CHECK(strstr(buf, "parent_record_id=7\n") != NULL);

    /* NULL fields render as '-' */
    e.capture_parameters = NULL;
    e.decoder_candidates = NULL;
    e.device_entity_id = NULL;
    e.operator_annotation = NULL;
    n = m1_env_format(buf, sizeof(buf), &e);
    CHECK(n > 0);
    CHECK(strstr(buf, "capture_parameters=-\n") != NULL);
    CHECK(strstr(buf, "decoder_candidates=-\n") != NULL);
    CHECK(strstr(buf, "device_entity_id=-\n") != NULL);
    CHECK(strstr(buf, "operator_annotation=-\n") != NULL);

    /* truncation contract: never half a record */
    CHECK(m1_env_format(buf, 16, &e) == 0);
    CHECK(m1_env_format(NULL, 1024, &e) == 0);

    /* --- session dir naming --- */
    n = m1_env_session_dir(buf, sizeof(buf), 0, "cafe0001");
    CHECK(n > 0);
    CHECK(strcmp(buf, "/Sessions/19700101-000000-cafe0001") == 0);

    n = m1_env_session_dir(buf, sizeof(buf), 1774812345, "ab12cd34");
    CHECK(n > 0);
    /* 1774812345 = 2026-03-29T19:25:45Z (leap-year date) */
    CHECK(strcmp(buf, "/Sessions/20260329-192545-ab12cd34") == 0);

    /* year boundary: 2026-12-31T23:59:59 = 1798761599 */
    n = m1_env_session_dir(buf, sizeof(buf), 1798761599, "00000001");
    CHECK(strcmp(buf, "/Sessions/20261231-235959-00000001") == 0);

    CHECK(m1_env_session_dir(buf, 12, 0, "x") == 0);   /* cap too small */

    /* --- session ids: stable length, deterministic, distinct --- */
    char a[9], b[9], c[9];
    m1_env_session_id(a, 123, 456);
    m1_env_session_id(b, 123, 456);
    m1_env_session_id(c, 124, 456);
    CHECK(strlen(a) == 8);
    CHECK(strcmp(a, b) == 0);
    CHECK(strcmp(a, c) != 0);

    if (g_failures == 0)
        printf("test_m1_env: ALL PASS\n");
    else
        printf("test_m1_env: %d FAILURES\n", g_failures);
    return g_failures ? 1 : 0;
}
