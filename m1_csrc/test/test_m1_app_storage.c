/* test_m1_app_storage.c — host tests for the storage umbrella (m1_app_storage.c)
 * and its path policy. Builds the REAL firmware source against the RAM-FatFs
 * stub; run: make -C m1_csrc/test test-storage
 *
 * Covers: sandbox resolution rules (absolute/relative, traversal, charset,
 * length, class), every op's happy path, atomic replace (no .part residue),
 * idempotent mkdir, list truncation flag, and every header-validation refusal.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"                 /* stub (include path order) */
#include "m1_app_syscall.h"

int m1_storage_resolve_path(uint8_t write_class, const char *in,
                            char *out, uint32_t out_cap);

static int g_failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0)

static void test_path_policy(void)
{
    char out[96];

    /* Relative paths resolve under the sandbox for both classes. */
    CHECK(m1_storage_resolve_path(0, "data.txt", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "0:/AppData/data.txt") == 0);
    CHECK(m1_storage_resolve_path(1, "sub/data.bin", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "0:/AppData/sub/data.bin") == 0);

    /* Absolute reads: allowed prefixes only, case-insensitive. */
    CHECK(m1_storage_resolve_path(0, "0:/apps/tool.m1app", out, sizeof(out)) == 0);
    CHECK(m1_storage_resolve_path(0, "0:/APPS/tool.m1app", out, sizeof(out)) == 0);
    CHECK(m1_storage_resolve_path(0, "0:/System/menu.cfg", out, sizeof(out)) == 0);
    CHECK(m1_storage_resolve_path(0, "0:/infrared/db/tv.ir", out, sizeof(out)) == 0);

    /* Absolute reads outside the allowlist: refused. */
    CHECK(m1_storage_resolve_path(0, "0:/nfc/secret.bin", out, sizeof(out)) == M1_SYS_ERR_DENIED);
    CHECK(m1_storage_resolve_path(0, "0:/subghz/capture.sgh", out, sizeof(out)) == M1_SYS_ERR_DENIED);

    /* Writes: absolute allowed ONLY inside the sandbox. */
    CHECK(m1_storage_resolve_path(1, "0:/AppData/x.txt", out, sizeof(out)) == 0);
    CHECK(m1_storage_resolve_path(1, "0:/appdata/x.txt", out, sizeof(out)) == 0);   /* case-insensitive */
    CHECK(m1_storage_resolve_path(1, "0:/apps/evil.m1app", out, sizeof(out)) == M1_SYS_ERR_DENIED);
    CHECK(m1_storage_resolve_path(1, "0:/System/menu.cfg", out, sizeof(out)) == M1_SYS_ERR_DENIED);

    /* Traversal and shape refusals. */
    CHECK(m1_storage_resolve_path(0, "../etc", out, sizeof(out)) == M1_SYS_ERR_DENIED);
    CHECK(m1_storage_resolve_path(0, "a/../b", out, sizeof(out)) == M1_SYS_ERR_DENIED);
    CHECK(m1_storage_resolve_path(0, "a//b", out, sizeof(out)) == M1_SYS_ERR_BAD_ARG);
    CHECK(m1_storage_resolve_path(0, "./x", out, sizeof(out)) == M1_SYS_ERR_BAD_ARG);
    CHECK(m1_storage_resolve_path(0, "a\\b", out, sizeof(out)) == M1_SYS_ERR_DENIED);
    CHECK(m1_storage_resolve_path(0, "a\x01b", out, sizeof(out)) == M1_SYS_ERR_DENIED);
    CHECK(m1_storage_resolve_path(0, "", out, sizeof(out)) == M1_SYS_ERR_BAD_ARG);
    CHECK(m1_storage_resolve_path(0, "0:/", out, sizeof(out)) == M1_SYS_ERR_BAD_ARG);
    CHECK(m1_storage_resolve_path(0, "a:b", out, sizeof(out)) == M1_SYS_ERR_DENIED);
    char longname[80];
    memset(longname, 'x', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    CHECK(m1_storage_resolve_path(0, longname, out, sizeof(out)) == M1_SYS_ERR_RANGE);
    CHECK(m1_storage_resolve_path(0, NULL, out, sizeof(out)) == M1_SYS_ERR_BAD_ARG);

    /* Trailing slash is trimmed, not fatal. */
    CHECK(m1_storage_resolve_path(0, "dir/", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "0:/AppData/dir") == 0);
}

static void fill_req(m1_sto_hdr_t *hdr, uint16_t size)
{
    memset(hdr, 0, size);
    hdr->size = size;
    hdr->version = M1_STO_LAYOUT_V1;
}

static void test_stat_write_read_append(void)
{
    hostff_reset();

    m1_sto_path_req_t preq;
    m1_sto_stat_resp_t sres;
    fill_req(&preq.hdr, sizeof(preq));
    strcpy(preq.path, "notes.txt");

    /* STAT on an absent relative file: success with exists=0. */
    memset(&sres, 0, sizeof(sres));
    CHECK(m1_storage_call(M1_STO_OP_STAT, &preq, sizeof(preq), &sres, sizeof(sres)) == 0);
    CHECK(sres.exists == 0 && sres.is_dir == 0 && sres.hdr.size == sizeof(sres));

    /* WRITE creates it atomically; no .part residue may survive. */
    m1_sto_write_req_t wreq;
    fill_req(&wreq.hdr, sizeof(wreq));
    strcpy(wreq.path, "notes.txt");
    memcpy(wreq.data, "hello", 5);
    wreq.length = 5;
    CHECK(m1_storage_call(M1_STO_OP_WRITE, &wreq, sizeof(wreq), NULL, 0) == 0);
    CHECK(hostff_bytes("0:/AppData/notes.txt") != NULL);
    CHECK(hostff_size("0:/AppData/notes.txt") == 5);
    CHECK(hostff_size("0:/AppData/notes.txt.part") == -1);

    /* WRITE again replaces (CREATE_ALWAYS path) with no residue. */
    memcpy(wreq.data, "world!!", 7);
    wreq.length = 7;
    CHECK(m1_storage_call(M1_STO_OP_WRITE, &wreq, sizeof(wreq), NULL, 0) == 0);
    CHECK(hostff_size("0:/AppData/notes.txt") == 7);
    CHECK(memcmp(hostff_bytes("0:/AppData/notes.txt"), "world!!", 7) == 0);
    CHECK(hostff_size("0:/AppData/notes.txt.part") == -1);

    /* STAT now sees it. */
    memset(&sres, 0, sizeof(sres));
    CHECK(m1_storage_call(M1_STO_OP_STAT, &preq, sizeof(preq), &sres, sizeof(sres)) == 0);
    CHECK(sres.exists == 1 && sres.file_size == 7 && sres.fresult == 0);

    /* APPEND grows the file, content preserved. */
    m1_sto_write_req_t areq;
    fill_req(&areq.hdr, sizeof(areq));
    strcpy(areq.path, "notes.txt");
    memcpy(areq.data, "-tail", 5);
    areq.length = 5;
    CHECK(m1_storage_call(M1_STO_OP_APPEND, &areq, sizeof(areq), NULL, 0) == 0);
    CHECK(hostff_size("0:/AppData/notes.txt") == 12);
    CHECK(memcmp(hostff_bytes("0:/AppData/notes.txt"), "world!!-tail", 12) == 0);

    /* READ from offset, short read at EOF returns bytes read. */
    m1_sto_read_req_t rreq;
    m1_sto_read_resp_t rres;
    fill_req(&rreq.hdr, sizeof(rreq));
    strcpy(rreq.path, "notes.txt");
    rreq.offset = 7;
    rreq.length = 100;                      /* clamped to IO_MAX, file has 5 left */
    memset(&rres, 0, sizeof(rres));
    int32_t got = m1_storage_call(M1_STO_OP_READ, &rreq, sizeof(rreq), &rres, sizeof(rres));
    CHECK(got == 5);
    CHECK(rres.length == 5 && memcmp(rres.data, "-tail", 5) == 0);

    /* READ of a missing file: IO error. */
    strcpy(rreq.path, "nope.txt");
    rreq.offset = 0;
    rreq.length = 4;
    CHECK(m1_storage_call(M1_STO_OP_READ, &rreq, sizeof(rreq), &rres, sizeof(rres)) == M1_SYS_ERR_IO);
    CHECK(rres.fresult == FR_NO_FILE);

    /* Reads from a library prefix work; writes there are denied. */
    uint8_t db[] = "IRDATA";
    hostff_put("0:/infrared/db/tv.ir", db, sizeof(db) - 1);
    fill_req(&rreq.hdr, sizeof(rreq));
    strcpy(rreq.path, "0:/infrared/db/tv.ir");
    rreq.offset = 0;
    rreq.length = 6;
    memset(&rres, 0, sizeof(rres));
    CHECK(m1_storage_call(M1_STO_OP_READ, &rreq, sizeof(rreq), &rres, sizeof(rres)) == 6);

    fill_req(&wreq.hdr, sizeof(wreq));
    strcpy(wreq.path, "0:/infrared/db/tv.ir");
    wreq.length = 1;
    CHECK(m1_storage_call(M1_STO_OP_WRITE, &wreq, sizeof(wreq), NULL, 0) == M1_SYS_ERR_DENIED);
    CHECK(m1_storage_call(M1_STO_OP_REMOVE, &wreq, sizeof(m1_sto_path_req_t), NULL, 0) != 0);
}

static void test_dirs_list_rename_remove(void)
{
    hostff_reset();

    /* MKDIR idempotent. */
    m1_sto_path_req_t preq;
    fill_req(&preq.hdr, sizeof(preq));
    strcpy(preq.path, "proj");
    CHECK(m1_storage_call(M1_STO_OP_MKDIR, &preq, sizeof(preq), NULL, 0) == 0);
    CHECK(m1_storage_call(M1_STO_OP_MKDIR, &preq, sizeof(preq), NULL, 0) == 0);

    m1_sto_write_req_t wreq;
    fill_req(&wreq.hdr, sizeof(wreq));
    wreq.length = 3;
    memcpy(wreq.data, "abc", 3);
    char names[5][8] = {"a", "b", "c", "d", "e"};
    char path[32];
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "proj/%s", names[i]);
        strcpy(wreq.path, path);
        CHECK(m1_storage_call(M1_STO_OP_WRITE, &wreq, sizeof(wreq), NULL, 0) == 0);
    }

    /* LIST with truncation: ask for 3 of 5. */
    m1_sto_list_req_t lreq;
    m1_sto_list_resp_t lres;
    fill_req(&lreq.hdr, sizeof(lreq));
    strcpy(lreq.path, "proj");
    lreq.max_entries = 3;
    memset(&lres, 0, sizeof(lres));
    int32_t n = m1_storage_call(M1_STO_OP_LIST, &lreq, sizeof(lreq), &lres, sizeof(lres));
    CHECK(n == 3);
    CHECK(lres.count == 3 && lres.total == 5);
    CHECK((lres.hdr.flags & M1_STO_FLAG_TRUNC) != 0);
    CHECK(lres.hdr.size == sizeof(lres));

    /* Full list clears the flag. */
    lreq.max_entries = M1_STO_LIST_MAX;
    memset(&lres, 0, sizeof(lres));
    n = m1_storage_call(M1_STO_OP_LIST, &lreq, sizeof(lreq), &lres, sizeof(lres));
    CHECK(n == 5 && lres.total == 5 && (lres.hdr.flags & M1_STO_FLAG_TRUNC) == 0);
    bool saw_dir = false, saw_file = false;
    for (uint16_t i = 0; i < lres.count; i++) {
        if (lres.entries[i].is_dir)
            saw_dir = true;
        else if (lres.entries[i].size == 3)
            saw_file = true;
    }
    /* 'proj' itself is listed from AppData root below, not here; all 5 are files. */
    CHECK(saw_file && !saw_dir);

    /* RENAME inside the sandbox. */
    m1_sto_rename_req_t rreq;
    fill_req(&rreq.hdr, sizeof(rreq));
    strcpy(rreq.from, "proj/a");
    strcpy(rreq.to, "proj/z");
    CHECK(m1_storage_call(M1_STO_OP_RENAME, &rreq, sizeof(rreq), NULL, 0) == 0);
    CHECK(hostff_size("0:/AppData/proj/z") == 3);
    CHECK(hostff_size("0:/AppData/proj/a") == -1);

    /* RENAME escaping the sandbox: denied either direction. */
    strcpy(rreq.from, "proj/z");
    strcpy(rreq.to, "0:/apps/evil");
    CHECK(m1_storage_call(M1_STO_OP_RENAME, &rreq, sizeof(rreq), NULL, 0) == M1_SYS_ERR_DENIED);
    strcpy(rreq.from, "0:/apps/tool.m1app");
    strcpy(rreq.to, "stolen");
    CHECK(m1_storage_call(M1_STO_OP_RENAME, &rreq, sizeof(rreq), NULL, 0) == M1_SYS_ERR_DENIED);

    /* REMOVE a file; REMOVE missing -> IO; remove non-empty dir -> IO. */
    strcpy(preq.path, "proj/b");
    CHECK(m1_storage_call(M1_STO_OP_REMOVE, &preq, sizeof(preq), NULL, 0) == 0);
    CHECK(hostff_size("0:/AppData/proj/b") == -1);
    CHECK(m1_storage_call(M1_STO_OP_REMOVE, &preq, sizeof(preq), NULL, 0) == M1_SYS_ERR_IO);
    strcpy(preq.path, "proj");
    CHECK(m1_storage_call(M1_STO_OP_REMOVE, &preq, sizeof(preq), NULL, 0) == M1_SYS_ERR_IO);
    strcpy(preq.path, "0:/System/menu.cfg");
    CHECK(m1_storage_call(M1_STO_OP_REMOVE, &preq, sizeof(preq), NULL, 0) == M1_SYS_ERR_DENIED);
}

static void test_header_validation(void)
{
    hostff_reset();
    m1_sto_path_req_t preq;
    m1_sto_stat_resp_t sres;

    /* Unknown op. */
    CHECK(m1_storage_call(99, &preq, sizeof(preq), NULL, 0) == M1_SYS_ERR_BAD_OP);

    /* NULL request / short request. */
    fill_req(&preq.hdr, sizeof(preq));
    strcpy(preq.path, "x");
    CHECK(m1_storage_call(M1_STO_OP_STAT, NULL, sizeof(preq), &sres, sizeof(sres)) == M1_SYS_ERR_BAD_ARG);
    CHECK(m1_storage_call(M1_STO_OP_STAT, &preq, 4, &sres, sizeof(sres)) == M1_SYS_ERR_BAD_ARG);

    /* Bad layout version, unknown flag bits. */
    fill_req(&preq.hdr, sizeof(preq));
    preq.hdr.version = 2;
    CHECK(m1_storage_call(M1_STO_OP_STAT, &preq, sizeof(preq), &sres, sizeof(sres)) == M1_SYS_ERR_BAD_ARG);
    preq.hdr.version = M1_STO_LAYOUT_V1;
    preq.hdr.flags = 0x80;
    CHECK(m1_storage_call(M1_STO_OP_STAT, &preq, sizeof(preq), &sres, sizeof(sres)) == M1_SYS_ERR_BAD_ARG);

    /* Response buffer too small. */
    preq.hdr.flags = 0;
    CHECK(m1_storage_call(M1_STO_OP_STAT, &preq, sizeof(preq), &sres, 4) == M1_SYS_ERR_RANGE);
    CHECK(m1_storage_call(M1_STO_OP_STAT, &preq, sizeof(preq), NULL, 0) == M1_SYS_ERR_RANGE);

    /* READ length bounds. */
    m1_sto_read_req_t rreq;
    fill_req(&rreq.hdr, sizeof(rreq));
    strcpy(rreq.path, "x");
    rreq.length = 0;
    CHECK(m1_storage_call(M1_STO_OP_READ, &rreq, sizeof(rreq), NULL, 0) == M1_SYS_ERR_RANGE);
    rreq.length = M1_STO_IO_MAX + 1;
    CHECK(m1_storage_call(M1_STO_OP_READ, &rreq, sizeof(rreq), NULL, 0) == M1_SYS_ERR_RANGE);

    /* LIST bounds. */
    m1_sto_list_req_t lreq;
    fill_req(&lreq.hdr, sizeof(lreq));
    strcpy(lreq.path, ".");
    lreq.max_entries = 0;
    CHECK(m1_storage_call(M1_STO_OP_LIST, &lreq, sizeof(lreq), NULL, 0) == M1_SYS_ERR_RANGE);
}

int main(void)
{
    test_path_policy();
    test_stat_write_read_append();
    test_dirs_list_rename_remove();
    test_header_validation();
    if (g_failures == 0)
        printf("test_m1_app_storage: ALL PASS\n");
    else
        printf("test_m1_app_storage: %d FAILURES\n", g_failures);
    return g_failures ? 1 : 0;
}
