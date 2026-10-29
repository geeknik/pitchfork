/* fuzz_m1_elf_loader.c — coverage-guided fuzz harness for the REAL ELF loader
 * (m1_elf_loader.c) via the RAM-FatFs stub. The property under test is purely
 * robustness: no input, however malformed, may crash, hang, read or write out
 * of bounds, or leak (ASan/UBSan run with the fuzzer). Build & run:
 *   make -C m1_csrc/test fuzz-elf   # clang + libFuzzer
 * The structured seeds in test_m1_elf_loader.c exercise the interesting
 * internals; this harness explores the surrounding input space.
 */
#include <stdint.h>
#include <string.h>

#include "ff.h"                 /* stub */
#include "FreeRTOS.h"           /* stub */
#include "task.h"               /* stub */
#include "m1_log_debug.h"       /* stub */
#include "m1_elf_loader.h"      /* real */

static m1_api_sym_t s_table[2];
static m1_api_interface_t s_api;

static int fuzz_fn_a(int x) { return x; }
static int fuzz_fn_b(int x) { return x; }

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    hostff_reset();
    s_table[0].name = "app_api_fn";
    s_table[0].address = (void *)(uintptr_t)&fuzz_fn_a;
    s_table[0].hash = elf_gnu_hash("app_api_fn");
    s_table[1].name = "weak_fn";
    s_table[1].address = (void *)(uintptr_t)&fuzz_fn_b;
    s_table[1].hash = elf_gnu_hash("weak_fn");
    if (s_table[1].hash < s_table[0].hash) {   /* keep the table hash-sorted */
        m1_api_sym_t t = s_table[0]; s_table[0] = s_table[1]; s_table[1] = t;
    }
    s_api.table = s_table;
    s_api.count = 2;
    s_api.api_version = M1_APP_API_VERSION;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char *path = "0:/apps/fuzz.m1app";
    hostff_reset();
    hostff_put(path, data, size);

    elf_app_t app;
    elf_load_status_t st = elf_load_app(path, &app, &s_api);
    if (st == ELF_OK)
        elf_unload_app(&app);
    return 0;
}
