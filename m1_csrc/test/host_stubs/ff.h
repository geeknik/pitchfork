/* ff.h — host-test stub of the FatFs R0.15 API subset used by m1_app_storage.c
 * and m1_elf_loader.c. Shadowed over the real ff.h via -I order in the test
 * Makefile. The implementation lives in host_stubs.c so every TU in a test
 * binary shares ONE in-memory tree; this header carries only types/prototypes.
 *
 * Test-facing extras (not part of FatFs):
 *   hostff_reset()           wipe the tree
 *   hostff_put(path,data,n)  create/replace a file with contents
 *   hostff_bytes(path)       NULL if absent, else pointer to file contents
 *   hostff_size(path)        file size, -1 if absent
 *   hostff_mkdir_raw(path)   create a directory
 */
#ifndef HOST_FF_STUB_H
#define HOST_FF_STUB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    FR_OK = 0, FR_DISK_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH,
    FR_INVALID_NAME, FR_DENIED, FR_EXIST, FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED, FR_INVALID_DRIVE, FR_NOT_ENABLED, FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED, FR_TIMEOUT, FR_OUT_OF_MEMORY, FR_TOO_MANY_OPEN_FILES,
    FR_INVALID_PARAMETER
} FRESULT;

typedef unsigned int UINT;
typedef uint32_t FSIZE_t;
typedef uint8_t BYTE;
typedef const char TCHAR;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_NEW 0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS 0x10
#define FA_OPEN_APPEND 0x30

#define AM_RDO 0x01
#define AM_HID 0x02
#define AM_SYS 0x04
#define AM_DIR 0x10

typedef struct {
    char fname[64];
    FSIZE_t fsize;
    BYTE fattrib;
} FILINFO;

typedef struct node {
    char name[48];
    bool is_dir;
    uint8_t *data;
    FSIZE_t size;
    FSIZE_t cap;
    struct node *child, *sibling, *parent;
    struct node *walk;      /* DIR cursor */
} node_t;

typedef struct { node_t *n; FSIZE_t pos; bool open; bool writable; } FIL;
typedef struct { node_t *n; } DIR;

/* ---- test helpers ---- */
void hostff_reset(void);
void hostff_put(const char *path, const uint8_t *data, size_t len);
FRESULT hostff_mkdir_raw(const char *path);
const uint8_t *hostff_bytes(const char *path);
long hostff_size(const char *path);

/* ---- FatFs API subset ---- */
FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buf, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_sync(FIL *fp);
FRESULT f_stat(const TCHAR *path, FILINFO *fno);
FRESULT f_unlink(const TCHAR *path);
FRESULT f_rename(const TCHAR *from, const TCHAR *to);
FRESULT f_mkdir(const TCHAR *path);
FRESULT f_opendir(DIR *dp, const TCHAR *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);

#endif /* HOST_FF_STUB_H */
