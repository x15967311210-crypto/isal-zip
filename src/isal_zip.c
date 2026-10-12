/* isal_zip: produce a real .zip archive whose payloads are compressed by Intel ISA-L.
 *
 * Ships two ways from the same source:
 *   isal_zip.exe   command-line packer
 *   isal_zip.dll   library module exporting isal_zip_pack() so other tools
 *                  (e.g. SAS via SASCBTBL / modulen) can pack a folder directly,
 *                  without shelling out to the exe.
 *
 * Supports: recursive folders, multiple threads, ZIP64 (for >4 GiB parts /
 * >65535 entries), the UTF-8 filename flag, explicit directory entries, and a
 * "store" fallback when Deflate does not shrink a part. Safe for OOXML/OPC.
 *
 * Usage (exe): isal_zip <level> <out.zip> [--root] <file-or-dir1> [<file-or-dir2> ...]
 *   <level>  ISA-L level: 0=store, 1=fastest, 2, 3=best
 *   --root   store a directory's *contents* at the archive root (needed for OOXML/OPC)
 * Env:
 *   ISAL_ZIP_THREADS=N   compress N files in parallel (default 1)
 *   ISAL_ZIP_FORCE64=1   always emit ZIP64 records (for testing)
 *   ISAL_ZIP_VERBOSE=1   print the one-line summary from the DLL (default: exe only)
 *
 * Library API (isal_zip.dll):
 *   int isal_zip_pack(const char *outZip, const char *rootDir, int level, int threads);
 *     outZip  : full path of the .zip to write
 *     rootDir : source directory; its *contents* go to the archive root and
 *               [Content_Types].xml (if present) is forced to be the first entry
 *     level   : ISA-L level 0..3
 *     threads : parallel worker threads (<=1 means single-threaded)
 *     returns 0 on success, non-zero on failure
 *   int isal_zip_rmtree(const char *dir);
 *     Recursively deletes `dir`. Returns 0 on success, also when the path does
 *     not exist (idempotent). Lets a host clean its scratch tree without
 *     spawning cmd.exe/rd.
 *
 * Build:
 *   gcc -O2 isal_zip.c <path>\isa-l.dll.a -I <build_include> -I <include> -o isal_zip.exe
 *   gcc -O2 -shared -DISAL_ZIP_DLL isal_zip.c <path>\isa-l.dll.a \
 *       -I <build_include> -I <include> -o isal_zip.dll
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>
#include <locale.h>
#include <windows.h>
#include <dirent.h>
#include <sys/stat.h>
#include "isa-l/igzip_lib.h"
#include "isa-l/crc.h"

#define NAMEMAX 1024
#define PATHMAX 4096
#define U32MAX_ 0xFFFFFFFFu
#define U16MAX_ 0xFFFFu
#define Z64_EXTRA_ID 0x0001u

/* Compile with -DISAL_ZIP_DLL to emit a shared library that exports
 * isal_zip_pack() instead of the isal_zip.exe entry point. */
#ifdef ISAL_ZIP_DLL
#  define ZIP_EXPORT __declspec(dllexport)
#else
#  define ZIP_EXPORT
#endif

static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put64(unsigned char *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static double now(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static const char *base_name(const char *p) {
    const char *r = p;
    for (; *p; p++) if (*p == '\\' || *p == '/') r = p + 1;
    return r;
}

/* set the general-purpose UTF-8 name flag (bit 11) for non-ASCII names */
static unsigned short gpflag_for(const char *name) {
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p >= 0x80) return 0x0800;
    return 0;
}

static uint32_t lvl_buf_size(uint32_t level) {
    switch (level) { case 1: return ISAL_DEF_LVL1_DEFAULT; case 2: return ISAL_DEF_LVL2_DEFAULT; default: return ISAL_DEF_LVL3_DEFAULT; }
}

typedef struct {
    char *path;               /* filesystem path, or NULL for a directory entry */
    uint64_t usize, csize, off;
    uint32_t crc;
    unsigned short method;    /* 0 = store, 8 = deflate */
    unsigned short gpflag;
    unsigned char *blob;
    char name[NAMEMAX];
} entry_t;

#ifdef ISAL_ZIP_DLL
static int g_verbose = 0;     /* quiet when loaded into a host process */
#else
static int g_verbose = 1;
#endif
static uint32_t g_level;
static int g_force64;
static int g_failed = 0;      /* set instead of exit() so a DLL never kills its host */
static entry_t *g_ents = NULL;
static int g_nfiles = 0, g_cap = 0;
static volatile LONG g_next;

static void reset_state(void) {
    for (int i = 0; i < g_nfiles; i++) { free(g_ents[i].path); free(g_ents[i].blob); }
    g_nfiles = 0;
    g_next = 0;
    g_failed = 0;
}

static int add_entry(const char *path, const char *arcname) {
    if (g_nfiles == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_ents = (entry_t *)realloc(g_ents, sizeof(entry_t) * g_cap);
        if (!g_ents) { fprintf(stderr, "oom\n"); g_failed = 1; return -1; }
    }
    entry_t *e = &g_ents[g_nfiles++];
    memset(e, 0, sizeof(*e));
    e->path = path ? _strdup(path) : NULL;
    strncpy(e->name, arcname, NAMEMAX - 1);
    e->gpflag = gpflag_for(e->name);
    return 0;
}

static int walk(const char *dirpath, const char *prefix) {
    DIR *d = opendir(dirpath);
    if (!d) { fprintf(stderr, "cannot open dir %s\n", dirpath); g_failed = 1; return -1; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[PATHMAX], rel[NAMEMAX], dirent_name[NAMEMAX];
        snprintf(full, sizeof(full), "%s/%s", dirpath, de->d_name);
        if (prefix[0]) snprintf(rel, sizeof(rel), "%s/%s", prefix, de->d_name);
        else snprintf(rel, sizeof(rel), "%s", de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            snprintf(dirent_name, sizeof(dirent_name), "%s/", rel);  /* explicit dir entry */
            if (add_entry(NULL, dirent_name) != 0) break;
            walk(full, rel);
        } else if (S_ISREG(st.st_mode)) {
            if (add_entry(full, rel) != 0) break;
        }
    }
    closedir(d);
    return g_failed ? -1 : 0;
}

static int collect(const char *arg, int root_mode) {
    struct stat st;
    if (stat(arg, &st) != 0) { fprintf(stderr, "not found: %s\n", arg); g_failed = 1; return -1; }
    if (S_ISDIR(st.st_mode)) return walk(arg, root_mode ? "" : base_name(arg));
    return add_entry(arg, base_name(arg));
}

/* read one file, CRC it, deflate it with ISA-L (or store if that does not help) */
static int prepare(int i) {
    entry_t *e = &g_ents[i];
    if (!e->path) { e->method = 0; return 0; }   /* directory entry: empty */

    FILE *in = fopen(e->path, "rb");
    if (!in) { perror(e->path); return -1; }
    fseek(in, 0, SEEK_END);
    long long n = _ftelli64(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)n + 1);
    if (!buf) { fclose(in); fprintf(stderr, "oom\n"); return -1; }
    if (fread(buf, 1, (size_t)n, in) != (size_t)n) { fprintf(stderr, "read err %s\n", e->path); fclose(in); free(buf); return -1; }
    fclose(in);

    uint32_t crc = crc32_gzip_refl(0, buf, (size_t)n);
    size_t cap = (size_t)n + (size_t)n / 1000 + 4096;
    unsigned char *cmp = (unsigned char *)malloc(cap);
    if (!cmp) { free(buf); fprintf(stderr, "oom\n"); return -1; }

    struct isal_zstream s;
    isal_deflate_stateless_init(&s);
    s.next_in = buf;   s.avail_in = (uint32_t)n;
    s.next_out = cmp;  s.avail_out = (uint32_t)cap;
    s.level = g_level;
    s.level_buf = (uint8_t *)malloc(lvl_buf_size(g_level));
    s.level_buf_size = lvl_buf_size(g_level);
    s.end_of_stream = 1; s.flush = NO_FLUSH; s.gzip_flag = IGZIP_DEFLATE;
    int rc = isal_deflate_stateless(&s);
    free(s.level_buf);
    if (rc != COMP_OK) { free(buf); free(cmp); fprintf(stderr, "deflate rc=%d\n", rc); return -1; }

    uint32_t csize = s.total_out;
    if (n > 0 && (size_t)csize >= (size_t)n) {
        /* Deflate did not help (e.g. already-compressed media): store raw. */
        free(cmp);
        e->blob = buf;
        e->csize = (uint64_t)n;
        e->method = 0;
    } else {
        free(buf);
        e->blob = cmp;
        e->csize = (uint64_t)csize;
        e->method = 8;
    }
    e->crc = crc;
    e->usize = (uint64_t)n;
    return 0;
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (;;) {
        LONG i = InterlockedIncrement(&g_next) - 1;
        if (i >= g_nfiles) break;
        if (prepare((int)i) != 0) return (DWORD)1;
    }
    return 0;
}

static int write_archive(FILE *out) {
    for (int i = 0; i < g_nfiles; i++) {
        entry_t *e = &g_ents[i];
        size_t nl = strlen(e->name);
        e->off = (uint64_t)_ftelli64(out);
        int z64s = g_force64 || e->usize >= U32MAX_ || e->csize >= U32MAX_;
        unsigned short vneed = z64s ? 45 : 20;

        unsigned char lfh[30];
        put32(lfh + 0, 0x04034b50u);
        put16(lfh + 4, vneed);
        put16(lfh + 6, e->gpflag);
        put16(lfh + 8, e->method);
        put16(lfh + 10, 0); put16(lfh + 12, 0x5A21);
        put32(lfh + 14, e->crc);
        put32(lfh + 18, z64s ? U32MAX_ : (uint32_t)e->csize);
        put32(lfh + 22, z64s ? U32MAX_ : (uint32_t)e->usize);
        put16(lfh + 26, (uint16_t)nl);
        put16(lfh + 28, z64s ? 20 : 0);
        fwrite(lfh, 1, 30, out);
        fwrite(e->name, 1, nl, out);
        if (z64s) {
            unsigned char x[20];
            put16(x + 0, Z64_EXTRA_ID); put16(x + 2, 16);
            put64(x + 4, e->usize); put64(x + 12, e->csize);
            fwrite(x, 1, 20, out);
        }
        if (e->csize) fwrite(e->blob, 1, (size_t)e->csize, out);
    }

    uint64_t cd_start = (uint64_t)_ftelli64(out);
    for (int i = 0; i < g_nfiles; i++) {
        entry_t *e = &g_ents[i];
        size_t nl = strlen(e->name);
        int zu = g_force64 || e->usize >= U32MAX_;
        int zc = g_force64 || e->csize >= U32MAX_;
        int zo = g_force64 || e->off   >= U32MAX_;
        int nz = zu + zc + zo;
        unsigned char x[4 + 8 * 3];
        uint16_t xl = 0;
        if (nz) {
            put16(x + 0, Z64_EXTRA_ID);
            put16(x + 2, (uint16_t)(nz * 8));
            uint16_t o = 4;
            if (zu) { put64(x + o, e->usize); o += 8; }
            if (zc) { put64(x + o, e->csize); o += 8; }
            if (zo) { put64(x + o, e->off);   o += 8; }
            xl = o;
        }
        unsigned char cdh[46];
        put32(cdh + 0, 0x02014b50u);
        put16(cdh + 4, nz ? 45 : 20);   /* version made by */
        put16(cdh + 6, nz ? 45 : 20);   /* version needed  */
        put16(cdh + 8, e->gpflag);
        put16(cdh + 10, e->method);
        put16(cdh + 12, 0); put16(cdh + 14, 0x5A21);
        put32(cdh + 16, e->crc);
        put32(cdh + 20, zc ? U32MAX_ : (uint32_t)e->csize);
        put32(cdh + 24, zu ? U32MAX_ : (uint32_t)e->usize);
        put16(cdh + 28, (uint16_t)nl);
        put16(cdh + 30, xl);
        put16(cdh + 32, 0);
        put16(cdh + 34, 0);
        put16(cdh + 36, 0);
        put32(cdh + 38, e->path ? 0x00000020u : 0x00000010u); /* DOS attr: file / dir */
        put32(cdh + 42, zo ? U32MAX_ : (uint32_t)e->off);
        fwrite(cdh, 1, 46, out);
        fwrite(e->name, 1, nl, out);
        if (nz) fwrite(x, 1, xl, out);
    }

    uint64_t cd_end = (uint64_t)_ftelli64(out);
    uint64_t cd_size = cd_end - cd_start;
    int need64 = g_force64 || g_nfiles >= U16MAX_ ||
                 cd_start >= U32MAX_ || cd_size >= U32MAX_;
    if (need64) {
        uint64_t here = (uint64_t)_ftelli64(out);
        unsigned char r[56];
        put32(r + 0, 0x06064b50u);
        put64(r + 4, 44);
        put16(r + 12, 45); put16(r + 14, 45);
        put32(r + 16, 0); put32(r + 20, 0);
        put64(r + 24, (uint64_t)g_nfiles);
        put64(r + 32, (uint64_t)g_nfiles);
        put64(r + 40, cd_size);
        put64(r + 48, cd_start);
        fwrite(r, 1, 56, out);
        unsigned char loc[20];
        put32(loc + 0, 0x07064b50u);
        put32(loc + 4, 0);
        put64(loc + 8, here);
        put32(loc + 16, 1);
        fwrite(loc, 1, 20, out);
    }
    unsigned char eocd[22];
    put32(eocd + 0, 0x06054b50u);
    put16(eocd + 4, 0); put16(eocd + 6, 0);
    put16(eocd + 8, need64 ? U16MAX_ : (uint16_t)g_nfiles);
    put16(eocd + 10, need64 ? U16MAX_ : (uint16_t)g_nfiles);
    put32(eocd + 12, need64 ? U32MAX_ : (uint32_t)cd_size);
    put32(eocd + 16, need64 ? U32MAX_ : (uint32_t)cd_start);
    put16(eocd + 20, 0);
    fwrite(eocd, 1, 22, out);
    return 0;
}

/* Shared core: pack `paths` into `outpath`. root_mode stores each directory's
 * *contents* at the archive root and fixes up the OPC first-entry rule. */
static int pack_archive(const char *outpath, int level, int threads, int force64,
                        const char *const *paths, int npaths, int root_mode) {
    reset_state();
    g_level = (uint32_t)level;
    g_force64 = force64;
    if (threads < 1) threads = 1;

    for (int i = 0; i < npaths; i++) {
        if (collect(paths[i], root_mode) != 0) return 1;
    }
    if (g_nfiles == 0) { fprintf(stderr, "no input files\n"); return 1; }

    /* OPC rule: the Content Types stream must be the first item in the archive */
    for (int i = 0; i < g_nfiles; i++) {
        if (!strcmp(g_ents[i].name, "[Content_Types].xml")) {
            if (i) { entry_t t = g_ents[0]; g_ents[0] = g_ents[i]; g_ents[i] = t; }
            break;
        }
    }

    if (threads > g_nfiles) threads = g_nfiles;

    FILE *out = fopen(outpath, "wb");
    if (!out) { perror("out"); return 1; }

    double t0 = now();
    if (threads <= 1) {
        for (int i = 0; i < g_nfiles; i++) {
            if (prepare(i) != 0) { fclose(out); return 1; }
        }
    } else {
        HANDLE *h = (HANDLE *)malloc(sizeof(HANDLE) * threads);
        if (!h) { fclose(out); fprintf(stderr, "oom\n"); return 1; }
        for (int t = 0; t < threads; t++) h[t] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        WaitForMultipleObjects(threads, h, TRUE, INFINITE);
        for (int t = 0; t < threads; t++) CloseHandle(h[t]);
        free(h);
    }
    int rc = write_archive(out);
    fclose(out);
    double dt = now() - t0;
    if (rc != 0) return 1;

    if (g_verbose) {
        unsigned long long tin = 0, tout = 0;
        int nent = 0;
        for (int i = 0; i < g_nfiles; i++) { tin += g_ents[i].usize; tout += g_ents[i].csize; if (g_ents[i].path) nent++; }
        printf("ISAL_ZIP lvl%u threads=%d files=%d in=%llu deflated=%llu ratio=%.3f time=%.4fs -> %s\n",
               g_level, threads, nent, tin, tout, tout ? (double)tin / (double)tout : 0.0, dt, outpath);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Library API (isal_zip.dll)                                          */
/* ------------------------------------------------------------------ */

/* Join a directory with a child name using a backslash. */
static wchar_t *wjoin(const wchar_t *dir, const wchar_t *child) {
    size_t ld = wcslen(dir), lc = wcslen(child);
    wchar_t *r = (wchar_t *)malloc(sizeof(wchar_t) * (ld + lc + 2));
    if (!r) return NULL;
    memcpy(r, dir, sizeof(wchar_t) * ld);
    r[ld] = L'\\';
    memcpy(r + ld + 1, child, sizeof(wchar_t) * (lc + 1));
    return r;
}

/* Recursive delete of one wide path. 1 = ok, 0 = failure. */
static int rmtree_w(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 1;          /* already gone */
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t *pat = wjoin(path, L"*");
        if (!pat) return 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        free(pat);
        int ok = 1;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                wchar_t *child = wjoin(path, fd.cFileName);
                if (!child) { ok = 0; break; }
                if (!rmtree_w(child)) ok = 0;
                free(child);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!DeleteFileW(path) && !RemoveDirectoryW(path)) ok = 0;
        return ok;
    }
    return DeleteFileW(path) ? 1 : (RemoveDirectoryW(path) ? 1 : 0);
}

/* Recursively delete `dir` (UTF-8 path). Returns 0 on success -- including when
 * the path does not exist -- so callers can use it as an idempotent cleanup. */
ZIP_EXPORT int isal_zip_rmtree(const char *dir) {
    if (!dir || !dir[0]) return 2;
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, NULL, 0);
    if (n <= 0) return 3;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * n);
    if (!w) return 3;
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, n);
    /* Never recurse through a reparse point: removing the link is enough. */
    DWORD attr = GetFileAttributesW(w);
    if (attr == INVALID_FILE_ATTRIBUTES) { free(w); return 0; }
    if ((attr & FILE_ATTRIBUTE_REPARSE_POINT) && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        int rc = RemoveDirectoryW(w) ? 0 : 1;
        free(w);
        return rc;
    }
    int rc = rmtree_w(w) ? 0 : 1;
    free(w);
    return rc;
}

/* Pack `rootDir`'s contents at the archive root, keeping [Content_Types].xml
 * first so the result is a valid OOXML/OPC package. Returns 0 on success. */
ZIP_EXPORT int isal_zip_pack(const char *outZip, const char *rootDir, int level, int threads) {
    if (!outZip || !rootDir || !outZip[0] || !rootDir[0]) return 2;
    setlocale(LC_ALL, ".UTF-8");   /* so directory names round-trip as UTF-8 */
    g_verbose = getenv("ISAL_ZIP_VERBOSE") != NULL;
    const char *paths[1] = { rootDir };
    int rc = pack_archive(outZip, level, threads, getenv("ISAL_ZIP_FORCE64") != NULL, paths, 1, 1);
    reset_state();                 /* free the blobs: the DLL may stay resident */
    return rc;
}

/* ------------------------------------------------------------------ */
/* Command-line entry point (isal_zip.exe)                             */
/* ------------------------------------------------------------------ */

#ifndef ISAL_ZIP_DLL
int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
            "usage: %s <level> <out.zip> [--root] <file-or-dir1> [more...]\n"
            "  --root : store directory contents at the archive root (needed for OOXML/OPC)\n",
            argv[0]);
        return 2;
    }
    setlocale(LC_ALL, ".UTF-8");   /* so directory names round-trip as UTF-8 */

    int level = atoi(argv[1]);
    const char *outpath = argv[2];
    int force64 = getenv("ISAL_ZIP_FORCE64") != NULL;

    int root_mode = 0;
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--root")) root_mode = 1;

    const char **paths = (const char **)malloc(sizeof(char *) * (argc - 3 + 1));
    if (!paths) { fprintf(stderr, "oom\n"); return 1; }
    int npaths = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--root")) continue;
        paths[npaths++] = argv[i];
    }

    const char *tenv = getenv("ISAL_ZIP_THREADS");
    int threads = tenv ? atoi(tenv) : 1;

    int rc = pack_archive(outpath, level, threads, force64, paths, npaths, root_mode);
    free(paths);
    reset_state();
    free(g_ents);
    g_ents = NULL; g_cap = 0;
    return rc;
}
#endif
