/* isal_zip: produce a real .zip archive whose payloads are compressed by Intel ISA-L.
 * Usage: isal_zip <level> <out.zip> <file-or-dir1> [<file-or-dir2> ...]
 *        directory args are walked recursively; entries are stored with the
 *        directory's base name as prefix (like `zip -r out.zip dir`).
 * Env  : ISAL_ZIP_THREADS=N   compress N files in parallel (default 1)
 * Writer: Local File Header + raw-deflate stream per file, then Central Directory + EOCD.
 * Build: gcc -O2 isal_zip.c <path>\isa-l.dll.a -I <build_include> -I <include> -o isal_zip.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include <dirent.h>
#include <sys/stat.h>
#include "isa-l/igzip_lib.h"
#include "isa-l/crc.h"

#define NAMEMAX 1024
#define PATHMAX 4096

static void put16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(unsigned char *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }

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

static uint32_t lvl_buf_size(uint32_t level) {
    switch (level) { case 1: return ISAL_DEF_LVL1_DEFAULT; case 2: return ISAL_DEF_LVL2_DEFAULT; default: return ISAL_DEF_LVL3_DEFAULT; }
}

typedef struct {
    char *path;               /* filesystem path */
    uint32_t crc, csize, usize, off;
    unsigned short method;    /* 0 = store, 8 = deflate */
    unsigned char *blob;      /* payload as written */
    char name[NAMEMAX];       /* name stored in archive ('/' separated) */
} entry_t;

static uint32_t g_level;
static entry_t *g_ents = NULL;
static int g_nfiles = 0, g_cap = 0;
static volatile LONG g_next;

static void add_entry(const char *path, const char *arcname) {
    if (g_nfiles == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_ents = (entry_t *)realloc(g_ents, sizeof(entry_t) * g_cap);
        if (!g_ents) { fprintf(stderr, "oom\n"); exit(1); }
    }
    entry_t *e = &g_ents[g_nfiles++];
    memset(e, 0, sizeof(*e));
    e->path = _strdup(path);
    strncpy(e->name, arcname, NAMEMAX - 1);
}

static void walk(const char *dirpath, const char *prefix) {
    DIR *d = opendir(dirpath);
    if (!d) { fprintf(stderr, "cannot open dir %s\n", dirpath); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[PATHMAX], rel[NAMEMAX];
        snprintf(full, sizeof(full), "%s/%s", dirpath, de->d_name);
        if (prefix[0]) snprintf(rel, sizeof(rel), "%s/%s", prefix, de->d_name);
        else snprintf(rel, sizeof(rel), "%s", de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) walk(full, rel);
        else if (S_ISREG(st.st_mode)) add_entry(full, rel);
    }
    closedir(d);
}

static void collect(const char *arg, int root_mode) {
    struct stat st;
    if (stat(arg, &st) != 0) { fprintf(stderr, "not found: %s\n", arg); exit(1); }
    if (S_ISDIR(st.st_mode)) {
        walk(arg, root_mode ? "" : base_name(arg));  /* --root: contents go to archive root */
    } else {
        add_entry(arg, base_name(arg));
    }
}

/* read one file, CRC it, deflate it with ISA-L into a fresh buffer */
static int prepare(int i) {
    const char *path = g_ents[i].path;
    FILE *in = fopen(path, "rb");
    if (!in) { perror(path); return -1; }
    fseek(in, 0, SEEK_END);
    long long n = _ftelli64(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, in) != (size_t)n) { fprintf(stderr, "read err %s\n", path); return -1; }
    fclose(in);

    uint32_t crc = crc32_gzip_refl(0, buf, (size_t)n);
    size_t cap = (size_t)n + (size_t)n / 1000 + 4096;
    unsigned char *cmp = (unsigned char *)malloc(cap);

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
        g_ents[i].blob = buf;            /* take ownership of the raw bytes */
        g_ents[i].csize = (uint32_t)n;
        g_ents[i].method = 0;
    } else {
        free(buf);
        g_ents[i].blob = cmp;
        g_ents[i].csize = csize;
        g_ents[i].method = 8;
    }
    g_ents[i].crc = crc;
    g_ents[i].usize = (uint32_t)n;
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
        size_t nl = strlen(g_ents[i].name);
        g_ents[i].off = (uint32_t)_ftelli64(out);
        unsigned char lfh[30];
        put32(lfh + 0, 0x04034b50);
        put16(lfh + 4, 20); put16(lfh + 6, 0); put16(lfh + 8, g_ents[i].method);
        put16(lfh + 10, 0); put16(lfh + 12, 0x5A21);
        put32(lfh + 14, g_ents[i].crc);
        put32(lfh + 18, g_ents[i].csize);
        put32(lfh + 22, g_ents[i].usize);
        put16(lfh + 26, (uint16_t)nl); put16(lfh + 28, 0);
        fwrite(lfh, 1, 30, out);
        fwrite(g_ents[i].name, 1, nl, out);
        if (g_ents[i].csize) fwrite(g_ents[i].blob, 1, g_ents[i].csize, out);
    }
    long cd_start = (long)_ftelli64(out);
    for (int i = 0; i < g_nfiles; i++) {
        size_t nl = strlen(g_ents[i].name);
        unsigned char cdh[46];
        put32(cdh + 0, 0x02014b50);
        put16(cdh + 4, 20); put16(cdh + 6, 20); put16(cdh + 8, 0); put16(cdh + 10, g_ents[i].method);
        put16(cdh + 12, 0); put16(cdh + 14, 0x5A21);
        put32(cdh + 16, g_ents[i].crc); put32(cdh + 20, g_ents[i].csize); put32(cdh + 24, g_ents[i].usize);
        put16(cdh + 28, (uint16_t)nl); put16(cdh + 30, 0); put16(cdh + 32, 0); put16(cdh + 34, 0);
        put16(cdh + 36, 0); put32(cdh + 38, 0); put32(cdh + 42, g_ents[i].off);
        fwrite(cdh, 1, 46, out);
        fwrite(g_ents[i].name, 1, nl, out);
    }
    long cd_end = (long)_ftelli64(out);
    unsigned char eocd[22];
    put32(eocd + 0, 0x06054b50);
    put16(eocd + 4, 0); put16(eocd + 6, 0);
    put16(eocd + 8, (uint16_t)g_nfiles); put16(eocd + 10, (uint16_t)g_nfiles);
    put32(eocd + 12, (uint32_t)(cd_end - cd_start));
    put32(eocd + 16, (uint32_t)cd_start);
    put16(eocd + 20, 0);
    fwrite(eocd, 1, 22, out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <level> <out.zip> [--root] <file-or-dir1> [more...]\n"
                                    "  --root : store directory contents at the archive root (needed for OOXML/OPC)\n", argv[0]); return 2; }
    g_level = (uint32_t)atoi(argv[1]);
    const char *outpath = argv[2];

    int root_mode = 0;
    for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--root")) root_mode = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--root")) continue;
        collect(argv[i], root_mode);
    }
    if (g_nfiles == 0) { fprintf(stderr, "no input files\n"); return 1; }
    /* OPC rule: the Content Types stream must be the first item in the archive */
    for (int i = 0; i < g_nfiles; i++) {
        if (!strcmp(g_ents[i].name, "[Content_Types].xml")) {
            if (i) { entry_t t = g_ents[0]; g_ents[0] = g_ents[i]; g_ents[i] = t; }
            break;
        }
    }

    const char *tenv = getenv("ISAL_ZIP_THREADS");
    int threads = tenv ? atoi(tenv) : 1;
    if (threads < 1) threads = 1;
    if (threads > g_nfiles) threads = g_nfiles;

    FILE *out = fopen(outpath, "wb");
    if (!out) { perror("out"); return 1; }

    double t0 = now();
    if (threads <= 1) {
        for (int i = 0; i < g_nfiles; i++) if (prepare(i) != 0) return 1;
    } else {
        HANDLE *h = (HANDLE *)malloc(sizeof(HANDLE) * threads);
        for (int t = 0; t < threads; t++) h[t] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        WaitForMultipleObjects(threads, h, TRUE, INFINITE);
        for (int t = 0; t < threads; t++) CloseHandle(h[t]);
        free(h);
    }
    int rc = write_archive(out);
    fclose(out);
    double dt = now() - t0;
    if (rc != 0) return 1;

    long long tin = 0, tout = 0;
    for (int i = 0; i < g_nfiles; i++) { tin += g_ents[i].usize; tout += g_ents[i].csize; }
    printf("ISAL_ZIP lvl%u threads=%d files=%d in=%lld deflated=%lld ratio=%.3f time=%.4fs -> %s\n",
           g_level, threads, g_nfiles, tin, tout, (double)tin / (double)tout, dt, outpath);
    return 0;
}
