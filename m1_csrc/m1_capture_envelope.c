/* See COPYING.txt for license details. */

/* m1_capture_envelope.c — pure envelope logic (host-tested). SD-side
 * writers live with the capture sources that use them. */

#include <stdio.h>
#include <string.h>
#include "m1_capture_envelope.h"

size_t m1_env_format(char *buf, size_t cap, const m1_envelope_t *e)
{
    if (!buf || cap == 0 || !e)
        return 0;
    int n = snprintf(buf, cap,
        "envelope_version=%d\n"
        "record_id=%lu\n"
        "session_id=%s\n"
        "timestamp_monotonic_ms=%lu\n"
        "source_domain=%s\n"
        "capture_parameters=%s\n"
        "raw_artifact_ref=%s\n"
        "decoder_candidates=%s\n"
        "confidence=%u\n"
        "device_entity_id=%s\n"
        "parent_record_id=%ld\n"
        "operator_annotation=%s\n"
        "integrity_hash=crc32c:%08lX\n",
        M1_ENV_VERSION,
        (unsigned long)e->record_id,
        e->session_id ? e->session_id : "-",
        (unsigned long)e->timestamp_monotonic_ms,
        e->source_domain ? e->source_domain : "-",
        e->capture_parameters ? e->capture_parameters : "-",
        e->raw_artifact_ref ? e->raw_artifact_ref : "-",
        e->decoder_candidates ? e->decoder_candidates : "-",
        (unsigned)e->confidence_pm,
        e->device_entity_id ? e->device_entity_id : "-",
        (long)(e->parent_record_id < 0 ? -1 : e->parent_record_id),
        e->operator_annotation ? e->operator_annotation : "-",
        (unsigned long)e->integrity_crc32c);
    if (n < 0 || (size_t)n >= cap)
        return 0;                       /* truncated — caller retries larger */
    return (size_t)n;
}

size_t m1_env_session_dir(char *out, size_t cap, uint32_t unix_sec,
                          const char *id8hex)
{
    /* Civil-time conversion from unix seconds (UTC): days-since-epoch to
     * Y/M/D via the standard civil-from-days algorithm — no libc time
     * dependency on device, identical on host. */
    int64_t days = unix_sec / 86400;
    int32_t rem = unix_sec % 86400;
    int h = rem / 3600, m = (rem % 3600) / 60, s = rem % 60;

    int64_t z = days + 719468;
    int32_t era = (int32_t)((z >= 0 ? z : z - 146096) / 146097);
    int64_t doe = z - era * 146097;
    int32_t yoe = (int32_t)((doe - doe/1460 + doe/36524 - doe/146096) / 365);
    int64_t y = yoe + era * 400;
    int32_t doy = (int32_t)(doe - (365*yoe + yoe/4 - yoe/100));
    int32_t mp = (5*doy + 2)/153;
    int32_t d = doy - (153*mp+2)/5 + 1;
    int32_t mo = mp + (mp < 10 ? 3 : -9);
    y += (mo <= 2);

    int n = snprintf(out, cap, "/Sessions/%04ld%02d%02d-%02d%02d%02d-%s",
                     (long)y, mo, d, h, m, s, id8hex);
    if (n < 0 || (size_t)n >= cap)
        return 0;
    return (size_t)n;
}

void m1_env_session_id(char out[9], uint32_t a, uint32_t b)
{
    /* FNV-1a mix of the entropy words -> 8 hex chars. */
    uint32_t h = 0x811C9DC5;
    for (int i = 0; i < 4; i++) {
        h ^= (a >> (i * 8)) & 0xFF; h *= 0x01000193;
        h ^= (b >> (i * 8)) & 0xFF; h *= 0x01000193;
    }
    snprintf(out, 9, "%08lx", (unsigned long)h);
}
