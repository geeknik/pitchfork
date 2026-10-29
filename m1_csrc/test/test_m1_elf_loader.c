/* test_m1_elf_loader.c — host robustness tests for the REAL ELF loader
 * (m1_elf_loader.c) running against the RAM-FatFs stub. Run:
 *   make -C m1_csrc/test test-elf
 *
 * Builds a minimal valid ET_REL ARM .m1app in memory (one .text, one .m1meta,
 * a symtab with app_main + strong/weak externals, a .rel.text with ABS32
 * relocations), then mutates it to hit every validation path: bad magic,
 * class, endianness, machine, type, section count clamp, manifest magic and
 * api gate, missing entry point, unresolved strong vs weak symbols, unknown
 * relocation type (documents the non-fatal skip), truncation, and open-failure
 * diagnostics via elf_last_ferr().
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"                 /* stub */
#include "FreeRTOS.h"           /* stub */
#include "task.h"               /* stub */
#include "m1_log_debug.h"       /* stub */
#include "m1_elf_loader.h"      /* real */

static int g_failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0)

/* ---------- image layout ---------- */
#define BLOB_SIZE   8192
#define OFF_EHDR    0x000
#define OFF_TEXT    0x100
#define OFF_META    0x120
#define OFF_REL     0x180
#define OFF_SYMTAB  0x1A0
#define OFF_STRTAB  0x200
#define OFF_SHSTRTAB 0x240
#define OFF_SHDR    0x400

#define SEC_NULL    0
#define SEC_TEXT    1
#define SEC_META    2
#define SEC_REL     3
#define SEC_SYMTAB  4
#define SEC_STRTAB  5
#define SEC_SHSTR   6
#define SHDR_COUNT  7

static uint8_t g_blob[BLOB_SIZE];

static void put8(uint32_t off, uint8_t v)  { g_blob[off] = v; }
static void put16(uint32_t off, uint16_t v) { memcpy(g_blob + off, &v, 2); }
static void put32(uint32_t off, uint32_t v) { memcpy(g_blob + off, &v, 4); }
static void putn(uint32_t off, const void *p, uint32_t n) { memcpy(g_blob + off, p, n); }

static void build_valid(void)
{
    memset(g_blob, 0, sizeof(g_blob));

    /* ELF header */
    put8(OFF_EHDR + 0, 0x7F); put8(OFF_EHDR + 1, 'E');
    put8(OFF_EHDR + 2, 'L');  put8(OFF_EHDR + 3, 'F');
    put8(OFF_EHDR + 4, 1);    /* ELFCLASS32 */
    put8(OFF_EHDR + 5, 1);    /* ELFDATA2LSB */
    put8(OFF_EHDR + 6, 1);    /* EV_CURRENT */
    put16(OFF_EHDR + 16, ET_REL);
    put16(OFF_EHDR + 18, EM_ARM);
    put32(OFF_EHDR + 20, 1);          /* e_version */
    put16(OFF_EHDR + 40, 52);         /* e_ehsize */
    put16(OFF_EHDR + 42, 0);          /* e_phentsize */
    put16(OFF_EHDR + 44, 0);          /* e_phnum */
    put16(OFF_EHDR + 46, 40);         /* e_shentsize */
    put16(OFF_EHDR + 48, SHDR_COUNT); /* e_shnum */
    put16(OFF_EHDR + 50, SEC_SHSTR);  /* e_shstrndx */
    put32(OFF_EHDR + 32, OFF_SHDR);   /* e_shoff */

    /* strtab: symbol names (offset 0 is the mandatory NUL byte) */
    g_blob[OFF_STRTAB] = 0;
    /* Manual layout (deterministic offsets, no cursor state across builds): */
    putn(OFF_STRTAB + 1, "app_main\0", 9);        /* offset 1 */
    putn(OFF_STRTAB + 10, "app_api_fn\0", 11);    /* offset 10 */
    putn(OFF_STRTAB + 21, "weak_fn\0", 8);        /* offset 21 */
    putn(OFF_STRTAB + 29, "missing_fn\0", 11);    /* offset 29 */
    putn(OFF_STRTAB + 40, "notmain\0", 8);        /* offset 40 */

    /* shstrtab */
    putn(OFF_SHSTRTAB + 1, ".text\0", 6);         /* offset 1 */
    putn(OFF_SHSTRTAB + 7, ".m1meta\0", 8);       /* offset 7 */
    putn(OFF_SHSTRTAB + 15, ".rel.text\0", 10);   /* offset 15 */
    putn(OFF_SHSTRTAB + 25, ".symtab\0", 8);      /* offset 25 */
    putn(OFF_SHSTRTAB + 33, ".strtab\0", 8);      /* offset 33 */
    putn(OFF_SHSTRTAB + 41, ".shstrtab\0", 10);   /* offset 41 */

    /* .text: 8 zero bytes; offsets 0 and 4 are ABS32 relocation sites */
    /* .m1meta: full 72-byte manifest */
    m1_app_manifest_t mf;
    memset(&mf, 0, sizeof(mf));
    mf.magic = M1_APP_MANIFEST_MAGIC;
    mf.api_version = 5;
    mf.stack_size = 512;
    strcpy(mf.name, "host");
    strcpy(mf.version, "1.0");
    putn(OFF_META, &mf, sizeof(mf));

    /* symtab: null, app_main (defined in .text), strong+weak externals */
    Elf32_Sym syms[5];
    memset(syms, 0, sizeof(syms));
    syms[1].st_name = 1;  syms[1].st_info = 0x12; syms[1].st_shndx = SEC_TEXT; syms[1].st_size = 4;
    syms[2].st_name = 10; syms[2].st_info = 0x12; syms[2].st_shndx = SHN_UNDEF;
    syms[3].st_name = 21; syms[3].st_info = 0x22; syms[3].st_shndx = SHN_UNDEF; /* STB_WEAK */
    putn(OFF_SYMTAB, syms, sizeof(syms));
    uint32_t symtab_size = sizeof(syms);

    /* .rel.text: ABS32 at .text+0 (strong external), .text+4 (weak external) */
    Elf32_Rel rels[2];
    memset(rels, 0, sizeof(rels));
    rels[0].r_offset = 0; rels[0].r_info = (2u << 8) | R_ARM_ABS32;
    rels[1].r_offset = 4; rels[1].r_info = (3u << 8) | R_ARM_ABS32;
    putn(OFF_REL, rels, sizeof(rels));

    /* section headers */
    Elf32_Shdr sh;
    memset(&sh, 0, sizeof(sh)); /* [0] NULL stays zero */
    putn(OFF_SHDR + 0 * sizeof(sh), &sh, sizeof(sh));

    sh.sh_name = 1;  sh.sh_type = SHT_PROGBITS; sh.sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    sh.sh_offset = OFF_TEXT; sh.sh_size = 8; sh.sh_addralign = 4;
    putn(OFF_SHDR + SEC_TEXT * sizeof(sh), &sh, sizeof(sh));

    sh.sh_name = 7;  sh.sh_type = SHT_PROGBITS; sh.sh_flags = SHF_ALLOC;
    sh.sh_offset = OFF_META; sh.sh_size = sizeof(m1_app_manifest_t); sh.sh_addralign = 4;
    putn(OFF_SHDR + SEC_META * sizeof(sh), &sh, sizeof(sh));

    sh.sh_name = 15; sh.sh_type = SHT_REL; sh.sh_flags = 0;
    sh.sh_offset = OFF_REL; sh.sh_size = sizeof(rels);
    sh.sh_link = SEC_SYMTAB; sh.sh_info = SEC_TEXT; sh.sh_addralign = 4; sh.sh_entsize = sizeof(Elf32_Rel);
    putn(OFF_SHDR + SEC_REL * sizeof(sh), &sh, sizeof(sh));

    sh.sh_name = 25; sh.sh_type = SHT_SYMTAB; sh.sh_flags = 0;
    sh.sh_offset = OFF_SYMTAB; sh.sh_size = symtab_size;
    sh.sh_link = SEC_STRTAB; sh.sh_info = 1; sh.sh_addralign = 4; sh.sh_entsize = sizeof(Elf32_Sym);
    putn(OFF_SHDR + SEC_SYMTAB * sizeof(sh), &sh, sizeof(sh));

    sh.sh_name = 33; sh.sh_type = SHT_STRTAB;
    sh.sh_offset = OFF_STRTAB; sh.sh_size = 64; sh.sh_addralign = 1;
    putn(OFF_SHDR + SEC_STRTAB * sizeof(sh), &sh, sizeof(sh));

    sh.sh_name = 41; sh.sh_type = SHT_STRTAB;
    sh.sh_offset = OFF_SHSTRTAB; sh.sh_size = 64; sh.sh_addralign = 1;
    putn(OFF_SHDR + SEC_SHSTR * sizeof(sh), &sh, sizeof(sh));
}

/* ---------- API table ---------- */

static int my_api_fn(int x) { return x + 1; }

static m1_api_sym_t s_table[1];
static m1_api_interface_t s_api;

static void api_init(void)
{
    s_table[0].name = "app_api_fn";
    s_table[0].address = (void *)(uintptr_t)&my_api_fn;
    s_table[0].hash = elf_gnu_hash("app_api_fn");
    s_api.table = s_table;
    s_api.count = 1;
    s_api.api_version = M1_APP_API_VERSION;
}

static const char *APP_PATH = "0:/apps/t.m1app";

static elf_load_status_t load_current(void)
{
    elf_app_t app;
    return elf_load_app(APP_PATH, &app, &s_api);
}

static void expect_status(const char *what, elf_load_status_t got, elf_load_status_t want)
{
    if (got != want)
        printf("  (%s: got %d want %d)\n", what, (int)got, (int)want);
    CHECK(got == want);
}

int main(void)
{
    hostff_reset();
    api_init();
    build_valid();
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);

    /* --- 1. valid image: loads, resolves, relocates, entry found --- */
    {
        elf_app_t app;
        elf_load_status_t st = elf_load_app(APP_PATH, &app, &s_api);
        expect_status("valid", st, ELF_OK);
        if (st == ELF_OK) {
            uint8_t *text = app.sections[SEC_TEXT].data;
            CHECK(text != NULL);
            CHECK(app.entry_point == ((uint32_t)(uintptr_t)text | 1u));
            CHECK(app.manifest.magic == M1_APP_MANIFEST_MAGIC);
            CHECK(strcmp(app.manifest.name, "host") == 0);
            CHECK(app.manifest.stack_size == 512);   /* within 256..16384, unclamped */
            uint32_t v0, v1;
            memcpy(&v0, text + 0, 4);
            memcpy(&v1, text + 4, 4);
            CHECK(v0 == (uint32_t)(uintptr_t)&my_api_fn);   /* ABS32 strong: += addr */
            CHECK(v1 == 0);                                 /* weak unresolved -> 0 */
            elf_unload_app(&app);
        }
    }

    /* --- 2. header refusals --- */
    build_valid(); put8(OFF_EHDR + 0, 0x00);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    expect_status("bad magic", load_current(), ELF_ERR_FORMAT);

    build_valid(); put8(OFF_EHDR + 4, 2);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    expect_status("class64", load_current(), ELF_ERR_FORMAT);

    build_valid(); put8(OFF_EHDR + 5, 2);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    expect_status("big-endian", load_current(), ELF_ERR_FORMAT);

    /* NB: elf_validate_header() itself rejects non-ARM machines before the
     * separate e_machine branch in elf_load_app() is reached, so the
     * ELF_ERR_MACHINE result is effectively dead code for this path. */
    build_valid(); put16(OFF_EHDR + 18, 3);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    expect_status("wrong machine", load_current(), ELF_ERR_FORMAT);

    build_valid(); put16(OFF_EHDR + 16, ET_EXEC);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    expect_status("not ET_REL", load_current(), ELF_ERR_FORMAT);

    build_valid(); put16(OFF_EHDR + 48, 0);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    expect_status("no sections", load_current(), ELF_ERR_FORMAT);

    /* --- 3. section-count clamp: 20 declared, 16 tracked, rest zeros --- */
    build_valid(); put16(OFF_EHDR + 48, 20);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    {
        elf_app_t app;
        elf_load_status_t st = elf_load_app(APP_PATH, &app, &s_api);
        expect_status("clamp sections", st, ELF_OK);
        if (st == ELF_OK) {
            CHECK(app.num_sections == ELF_MAX_SECTIONS);
            elf_unload_app(&app);
        }
    }

    /* --- 4. manifest refusals --- */
    build_valid(); put32(OFF_META + 0, 0xDEADBEEF);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    expect_status("bad manifest magic", load_current(), ELF_ERR_MANIFEST);

    build_valid(); put16(OFF_META + 4, 99);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    expect_status("api too new", load_current(), ELF_ERR_API_VERSION);

    /* --- 5. entry point missing (app_main renamed) --- */
    build_valid(); put32(OFF_SYMTAB + sizeof(Elf32_Sym) * 1 + 0, 40); /* name -> "notmain" */
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    CHECK(load_current() != ELF_OK);

    /* --- 6. unresolved STRONG external is fatal; weak is not --- */
    build_valid(); put32(OFF_SYMTAB + sizeof(Elf32_Sym) * 2 + 0, 29); /* -> "missing_fn" */
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    expect_status("unresolved strong", load_current(), ELF_ERR_SYMBOL);

    /* --- 7. unknown relocation type: non-fatal, site left unpatched --- */
    build_valid(); put32(OFF_REL + 4, (2u << 8) | 0x77);
    hostff_put(APP_PATH, g_blob, BLOB_SIZE);
    {
        elf_app_t app;
        elf_load_status_t st = elf_load_app(APP_PATH, &app, &s_api);
        expect_status("unknown reloc type", st, ELF_OK);
        if (st == ELF_OK) {
            uint32_t v0;
            memcpy(&v0, app.sections[SEC_TEXT].data, 4);
            CHECK(v0 == 0);   /* documented hazard: silently skipped */
            elf_unload_app(&app);
        }
    }

    /* --- 7b. relocation sites outside/straddling the section are skipped ---
     * Regression: the loader used to apply r_offset with no bounds check,
     * giving a crafted .m1app an arbitrary heap read-modify-write (found by
     * this suite under ASan on its first run). */
    {
        uint32_t bad_offsets[] = { 6, 8, 9, 0x277, 0xFFFF };
        for (size_t i = 0; i < sizeof(bad_offsets) / sizeof(bad_offsets[0]); i++) {
            build_valid();
            put32(OFF_REL + 0, bad_offsets[i]);          /* r_offset only */
            hostff_put(APP_PATH, g_blob, BLOB_SIZE);
            elf_app_t app;
            elf_load_status_t st = elf_load_app(APP_PATH, &app, &s_api);
            if (st == ELF_OK) {
                uint32_t v0;
                memcpy(&v0, app.sections[SEC_TEXT].data, 4);
                CHECK(v0 == 0);                          /* nothing was patched */
                elf_unload_app(&app);
            } else {
                printf("  (bad offset 0x%X: unexpected status %d)\n",
                       bad_offsets[i], (int)st);
                CHECK(st == ELF_OK);
            }
        }
    }

    /* --- 8. truncation at several cut points --- */
    size_t cuts[] = { 0, 10, 51, 52, 0x140, 0x200, OFF_SHDR + 2 * 40 };
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        build_valid();
        hostff_put(APP_PATH, g_blob, cuts[i]);
        expect_status("truncated", load_current(), ELF_ERR_FILE);
    }

    /* --- 9. open failure carries the FatFs code --- */
    {
        elf_app_t app;
        elf_load_status_t st = elf_load_app("0:/apps/none.m1app", &app, &s_api);
        expect_status("missing file", st, ELF_ERR_FILE);
        CHECK(elf_last_ferr() == FR_NO_FILE);
    }

    if (g_failures == 0)
        printf("test_m1_elf_loader: ALL PASS\n");
    else
        printf("test_m1_elf_loader: %d FAILURES\n", g_failures);
    return g_failures ? 1 : 0;
}
