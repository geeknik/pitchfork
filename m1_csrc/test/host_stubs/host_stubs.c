/* host_stubs.c — the ONE shared implementation behind the host-test FatFs stub
 * (ff.h) plus the m1_logdb_printf symbol the real m1_log_debug.h declares.
 * Keeping the tree here (not static-in-header) means every TU in a test binary
 * — test main, m1_app_storage.c, m1_elf_loader.c — sees the same filesystem. */
#include "ff.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "stm32h5xx_hal.h"     /* stub: provides _ATTRIBUTE */
#include "m1_log_debug_defs.h"

/* ---------------- tree ---------------- */

static node_t *g_root;

void hostff_reset(void)
{
    /* leak intentionally: process-lifetime test tree */
    g_root = calloc(1, sizeof(node_t));
    g_root->is_dir = true;
    strcpy(g_root->name, "/");
}

static node_t *walk(node_t *dir, const char *name)
{
    for (node_t *c = dir->child; c; c = c->sibling)
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

static node_t *lookup(const char *path)
{
    if (!g_root)
        hostff_reset();
    if (path[0] == '0' && path[1] == ':')
        path += 2;
    if (path[0] == '\0')
        return g_root;
    node_t *cur = g_root;
    char seg[48];
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        int n = 0;
        while (*p && *p != '/' && n < 47)
            seg[n++] = *p++;
        seg[n] = '\0';
        while (*p && *p != '/')
            p++;                                  /* skip overlong remainder */
        if (!cur->is_dir)
            return NULL;
        cur = walk(cur, seg);
        if (!cur)
            return NULL;
    }
    return cur;
}

static node_t *parent_of(const char *path, char *leaf_out, size_t leaf_cap)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", path);
    char *slash = strrchr(buf, '/');
    if (!slash)
        return NULL;
    snprintf(leaf_out, leaf_cap, "%s", slash + 1);
    if (slash == buf)
        return g_root;
    *slash = '\0';
    node_t *parent = lookup(buf[0] ? buf : "/");
    return (parent && parent->is_dir) ? parent : NULL;
}

static node_t *mkchild(node_t *dir, const char *name, bool is_dir)
{
    node_t *n = calloc(1, sizeof(node_t));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->is_dir = is_dir;
    n->parent = dir;
    n->sibling = dir->child;
    dir->child = n;
    return n;
}

/* ---------------- test helpers ---------------- */

void hostff_put(const char *path, const uint8_t *data, size_t len)
{
    char leaf[48];
    node_t *parent = parent_of(path, leaf, sizeof(leaf));
    if (!parent) {
        /* mkdir -p the missing chain, then retry once */
        char buf[128];
        snprintf(buf, sizeof(buf), "%s", path);
        if (buf[0] == '0' && buf[1] == ':')
            memmove(buf, buf + 2, strlen(buf + 2) + 1);
        for (char *p = buf; *p; p++) {
            if (*p == '/' && p != buf) {
                *p = '\0';
                if (lookup(buf[0] ? buf : "/") == NULL)
                    hostff_mkdir_raw(buf[0] ? buf : "/");
                *p = '/';
            }
        }
        parent = parent_of(path, leaf, sizeof(leaf));
    }
    if (!parent) {
        fprintf(stderr, "hostff_put: no parent for %s\n", path);
        return;
    }
    node_t *n = walk(parent, leaf);
    if (n && n->is_dir) {
        fprintf(stderr, "hostff_put: %s is a dir\n", path);
        return;
    }
    if (!n)
        n = mkchild(parent, leaf, false);
    free(n->data);
    n->cap = n->size = (FSIZE_t)len;
    n->data = malloc(len ? len : 1);
    if (len)
        memcpy(n->data, data, len);
}

FRESULT hostff_mkdir_raw(const char *path)
{
    char leaf[48];
    node_t *parent = parent_of(path, leaf, sizeof(leaf));
    if (!parent)
        return FR_NO_PATH;
    if (walk(parent, leaf))
        return FR_EXIST;
    mkchild(parent, leaf, true);
    return FR_OK;
}

const uint8_t *hostff_bytes(const char *path)
{
    node_t *n = lookup(path);
    return (n && !n->is_dir) ? n->data : NULL;
}

long hostff_size(const char *path)
{
    node_t *n = lookup(path);
    return (n && !n->is_dir) ? (long)n->size : -1;
}

/* ---------------- FatFs API ---------------- */

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    memset(fp, 0, sizeof(*fp));
    node_t *n = lookup(path);
    bool want_write = mode & FA_WRITE;

    if (n && n->is_dir)
        return FR_DENIED;
    if (!n && !(mode & (FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS)))
        return FR_NO_FILE;
    if (!n) {
        char leaf[48];
        node_t *parent = parent_of(path, leaf, sizeof(leaf));
        if (!parent)
            return FR_NO_PATH;
        n = mkchild(parent, leaf, false);
    }
    if ((mode & FA_CREATE_ALWAYS) && n->size)
        n->size = 0;
    if (mode & FA_OPEN_APPEND)
        fp->pos = n->size;
    if (want_write && n->cap == 0) {
        n->cap = 64;
        n->data = realloc(n->data, n->cap);
    }
    fp->n = n;
    fp->open = true;
    fp->writable = want_write;
    return FR_OK;
}

FRESULT f_close(FIL *fp)
{
    fp->open = false;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT btr, UINT *br)
{
    *br = 0;
    if (!fp->open || !fp->n)
        return FR_INVALID_OBJECT;
    FSIZE_t avail = (fp->n->size > fp->pos) ? fp->n->size - fp->pos : 0;
    UINT take = (btr < avail) ? btr : (UINT)avail;
    if (take)
        memcpy(buf, fp->n->data + fp->pos, take);
    fp->pos += take;
    *br = take;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buf, UINT btw, UINT *bw)
{
    *bw = 0;
    if (!fp->open || !fp->n || !fp->writable)
        return FR_INVALID_OBJECT;
    if (fp->pos + btw > fp->n->cap) {
        FSIZE_t ncap = (fp->pos + btw + 63) & ~(FSIZE_t)63;
        uint8_t *nd = realloc(fp->n->data, ncap);
        if (!nd)
            return FR_DENIED;
        fp->n->data = nd;
        fp->n->cap = ncap;
    }
    memcpy(fp->n->data + fp->pos, buf, btw);
    fp->pos += btw;
    if (fp->pos > fp->n->size)
        fp->n->size = fp->pos;
    *bw = btw;
    return FR_OK;
}

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    if (!fp->open || !fp->n)
        return FR_INVALID_OBJECT;
    fp->pos = ofs;
    return FR_OK;
}

FRESULT f_sync(FIL *fp) { (void)fp; return FR_OK; }

FRESULT f_stat(const TCHAR *path, FILINFO *fno)
{
    node_t *n = lookup(path);
    if (!n)
        return FR_NO_FILE;
    memset(fno, 0, sizeof(*fno));
    snprintf(fno->fname, sizeof(fno->fname), "%s", n->name);
    fno->fsize = n->size;
    fno->fattrib = n->is_dir ? AM_DIR : 0;
    return FR_OK;
}

FRESULT f_unlink(const TCHAR *path)
{
    node_t *n = lookup(path);
    if (!n)
        return FR_NO_FILE;
    if (n->is_dir && n->child)
        return FR_DENIED;
    if (n == g_root)
        return FR_DENIED;
    node_t **pp = &n->parent->child;
    while (*pp != n)
        pp = &(*pp)->sibling;
    *pp = n->sibling;
    return FR_OK;
}

FRESULT f_rename(const TCHAR *from, const TCHAR *to)
{
    node_t *src = lookup(from);
    if (!src)
        return FR_NO_FILE;
    if (lookup(to))
        return FR_EXIST;
    char leaf[48];
    node_t *dst_parent = parent_of(to, leaf, sizeof(leaf));
    if (!dst_parent)
        return FR_NO_PATH;
    node_t **pp = &src->parent->child;
    while (*pp != src)
        pp = &(*pp)->sibling;
    *pp = src->sibling;
    src->parent = dst_parent;
    src->sibling = dst_parent->child;
    dst_parent->child = src;
    snprintf(src->name, sizeof(src->name), "%s", leaf);
    return FR_OK;
}

FRESULT f_mkdir(const TCHAR *path)
{
    node_t *n = lookup(path);
    if (n)
        return FR_EXIST;
    return hostff_mkdir_raw(path);
}

FRESULT f_opendir(DIR *dp, const TCHAR *path)
{
    node_t *n = lookup(path);
    if (!n)
        return FR_NO_PATH;
    if (!n->is_dir)
        return FR_INVALID_NAME;
    dp->n = n;
    n->walk = n->child;
    return FR_OK;
}

FRESULT f_readdir(DIR *dp, FILINFO *fno)
{
    memset(fno, 0, sizeof(*fno));
    if (!dp->n)
        return FR_INVALID_OBJECT;
    node_t *cur = dp->n->walk;
    if (!cur) {
        fno->fname[0] = '\0';
        return FR_OK;
    }
    dp->n->walk = cur->sibling;
    snprintf(fno->fname, sizeof(fno->fname), "%s", cur->name);
    fno->fsize = cur->size;
    fno->fattrib = cur->is_dir ? AM_DIR : 0;
    return FR_OK;
}

FRESULT f_closedir(DIR *dp) { (void)dp; return FR_OK; }

/* ---------------- real-log-header symbol ---------------- */

void m1_logdb_printf(S_M1_LogDebugLevel_t lvl, const char *tag,
                     const char *format, ...)
{
    (void)lvl;
    va_list ap;
    va_start(ap, format);
    fprintf(stderr, "[m1][%s] ", tag ? tag : "?");
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}
