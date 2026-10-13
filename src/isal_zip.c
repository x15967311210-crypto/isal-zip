/* isal_zip: produce a real .zip archive whose payloads are compressed by Intel ISA-L.
 *
 * Ships two ways from the same source:
 *   isal_zip.exe   command-line packer
 *   isal_zip.dll   library module exporting isal_zip_pack() so other tools
 *                  (e.g. SAS via SASCBTBL / modulen) can pack a folder directly,
 *                  without shelling out to the exe.
 *
 * Supports: recursive folders, multiple threads (including splitting one large
 * file into independent Deflate blocks compressed in parallel), ZIP64 (for
 * >4 GiB parts / >65535 entries), the UTF-8 filename flag, explicit directory
 * entries, and a "store" fallback when Deflate does not shrink a part. Safe for
 * OOXML/OPC.
 *
 * Usage (exe): isal_zip <level> <out.zip> [--root] <file-or-dir1> [<file-or-dir2> ...]
 *   <level>  ISA-L level: 0=store, 1=fastest, 2, 3=best
 *   --root   store a directory's *contents* at the archive root (needed for OOXML/OPC)
 * Env:
 *   ISAL_ZIP_THREADS=N   run N workers: files in parallel, and one large file
 *                        split into N chunks (default 1)
 *   ISAL_ZIP_CHUNK=BYTES cap on the chunk size when splitting a large file
 *                        (default 4 MiB; ratio/speed trade-off)
 *   ISAL_ZIP_SPLIT=0     disable single-file splitting (file-level threads only)
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
 * Both exports pin the module on first use (GET_MODULE_HANDLE_EX_FLAG_PIN), so a
 * host that loads/unloads the DLL per call (SAS MODULE per DATA step) keeps it
 * resident instead of re-loading it -- and isa-l.dll with it -- every step.
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
#include <windows.h>
#include "isa-l/igzip_lib.h"
#include "isa-l/crc.h"

#define NAMEMAX 1024
#define U32MAX_ 0xFFFFFFFFu
#define U16MAX_ 0xFFFFu
#define Z64_EXTRA_ID 0x0001u

/* One file larger than the file-level thread pool is split into independent
 * Deflate blocks that are compressed in parallel and concatenated (the same
 * trick pigz uses). CHUNK_MIN bounds how finely we split (smaller chunks lose
 * more cross-chunk match history, i.e. ratio); CHUNK_MAX_DEFAULT caps the chunk
 * so the split cannot grow the archive too much. Both are overridable with
 * ISAL_ZIP_CHUNK=<bytes>; ISAL_ZIP_SPLIT=0 disables splitting entirely. */
#define CHUNK_MIN (1u << 20)
#define CHUNK_MAX_DEFAULT (4u << 20)

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

/* UTF-8 (as stored in the archive and passed on the command line) -> UTF-16 for
 * the Win32 file APIs. Caller frees. */
static wchar_t *u8_to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

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

/* Open a file for reading and return its true 64-bit size. One CreateFile +
 * GetFileSizeEx replaces the fopen/seek/seek/tell dance, and FILE_FLAG_
 * SEQUENTIAL_SCAN tells the cache manager how we will read it. */
static HANDLE open_file_size(const wchar_t *path, uint64_t *size) {
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    *size = (uint64_t)li.QuadPart;
    return h;
}

/* ReadFile's length is a DWORD, so loop until `n` bytes are in. Reading the
 * whole file in one (or a few) calls avoids CRT stdio's 4 KiB bounce buffer. */
static int read_all(HANDLE h, unsigned char *buf, uint64_t n) {
    uint64_t done = 0;
    while (done < n) {
        uint64_t left = n - done;
        DWORD want = (DWORD)(left > 0x40000000u ? 0x40000000u : left);
        DWORD got = 0;
        if (!ReadFile(h, buf + done, want, &got, NULL) || got == 0) return -1;
        done += got;
    }
    return 0;
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
    wchar_t *path;            /* filesystem path (UTF-16), or NULL for a directory entry */
    uint64_t fsize;           /* on-disk size (0 for directory entries) */
    uint64_t usize, csize, off;
    uint32_t crc;
    unsigned short method;    /* 0 = store, 8 = deflate */
    unsigned short gpflag;
    unsigned char *blob;
    int done;                 /* already prepared (big-file phase) */
    char name[NAMEMAX];
} entry_t;

/* Per-thread scratch, reused across files so we do not malloc/free a ~276-340 KB
 * ISA-L level buffer (and the input buffer) for every part of an OOXML package. */
typedef struct {
    unsigned char *in;   size_t in_cap;
    unsigned char *level; size_t level_cap;
} scratch_t;

#ifdef ISAL_ZIP_DLL
static int g_verbose = 0;     /* quiet when loaded into a host process */
#else
static int g_verbose = 1;
#endif
static uint32_t g_level;
static int g_force64;
static size_t g_chunk_max = CHUNK_MAX_DEFAULT;   /* upper bound on split chunk size */
static int g_split = 1;                          /* ISAL_ZIP_SPLIT=0 disables it */
static int g_failed = 0;      /* set instead of exit() so a DLL never kills its host */
static entry_t *g_ents = NULL;
static int g_nfiles = 0, g_cap = 0;
static volatile LONG g_next;

static unsigned char *scratch_ensure(unsigned char **p, size_t *cap, size_t need) {
    if (*p && *cap >= need) return *p;
    unsigned char *np = (unsigned char *)realloc(*p, need);
    if (!np) return NULL;         /* keep the old block; caller frees on teardown */
    *p = np; *cap = need;
    return np;
}

static void scratch_free(scratch_t *sb) {
    free(sb->in); free(sb->level);
    sb->in = NULL; sb->level = NULL; sb->in_cap = sb->level_cap = 0;
}

static void reset_state(void) {
    for (int i = 0; i < g_nfiles; i++) { free(g_ents[i].path); free(g_ents[i].blob); }
    g_nfiles = 0;
    g_next = 0;
    g_failed = 0;
}

static int add_entry(const wchar_t *path, const char *arcname, uint64_t fsize) {
    if (g_nfiles == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_ents = (entry_t *)realloc(g_ents, sizeof(entry_t) * g_cap);
        if (!g_ents) { fprintf(stderr, "oom\n"); g_failed = 1; return -1; }
    }
    entry_t *e = &g_ents[g_nfiles++];
    memset(e, 0, sizeof(*e));
    e->path = path ? _wcsdup(path) : NULL;
    if (path && !e->path) { g_nfiles--; fprintf(stderr, "oom\n"); g_failed = 1; return -1; }
    e->fsize = fsize;
    strncpy(e->name, arcname, NAMEMAX - 1);
    e->gpflag = gpflag_for(e->name);
    return 0;
}

/* Enumerate with FindFirstFileW/FindNextFileW: one call per entry yields both
 * the attributes and the full 64-bit size, so there is no separate stat(). */
static int walk(const wchar_t *dirw, const char *prefix) {
    wchar_t *pat = wjoin(dirw, L"*");
    if (!pat) { g_failed = 1; return -1; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "cannot open dir\n"); g_failed = 1; return -1; }
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char name_u8[NAMEMAX], rel[NAMEMAX], dirent_name[NAMEMAX];
        /* NAMEMAX holds the worst case (a 255-character NTFS component in
         * 4-byte UTF-8); a name we cannot encode is an error, not a silently
         * dropped entry. */
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name_u8, (int)sizeof(name_u8), NULL, NULL)) {
            fprintf(stderr, "cannot encode a file name as UTF-8\n");
            g_failed = 1; break;
        }
        int need = prefix[0]
            ? snprintf(rel, sizeof(rel), "%s/%s", prefix, name_u8)
            : snprintf(rel, sizeof(rel), "%s", name_u8);
        if (need < 0 || (size_t)need >= sizeof(rel)) {
            fprintf(stderr, "archive path too long (>%u bytes)\n", (unsigned)sizeof(rel) - 1);
            g_failed = 1; break;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int dn = snprintf(dirent_name, sizeof(dirent_name), "%s/", rel);  /* explicit dir entry */
            if (dn < 0 || (size_t)dn >= sizeof(dirent_name)) {
                fprintf(stderr, "archive path too long\n"); g_failed = 1; break;
            }
            if (add_entry(NULL, dirent_name, 0) != 0) break;
            wchar_t *child = wjoin(dirw, fd.cFileName);
            if (!child) { g_failed = 1; break; }
            int r = walk(child, rel);
            free(child);
            if (r != 0) break;
        } else {
            wchar_t *child = wjoin(dirw, fd.cFileName);
            if (!child) { g_failed = 1; break; }
            uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            int r = add_entry(child, rel, sz);
            free(child);
            if (r != 0) break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return g_failed ? -1 : 0;
}

static int collect(const char *arg, int root_mode) {
    wchar_t *argw = u8_to_wide(arg);
    if (!argw) { fprintf(stderr, "not found: %s\n", arg); g_failed = 1; return -1; }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(argw, GetFileExInfoStandard, &fad)) {
        fprintf(stderr, "not found: %s\n", arg); free(argw); g_failed = 1; return -1;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        int r = walk(argw, root_mode ? "" : base_name(arg));
        free(argw);
        return r;
    }
    uint64_t sz = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    int r = add_entry(argw, base_name(arg), sz);
    free(argw);
    return r;
}

/* read one file, CRC it, deflate it with ISA-L (or store if that does not help).
 * `sb` is a per-thread scratch reused across files. */
static int prepare(int i, scratch_t *sb) {
    entry_t *e = &g_ents[i];
    if (!e->path) { e->method = 0; return 0; }   /* directory entry: empty */

    uint64_t n64 = 0;
    HANDLE in = open_file_size(e->path, &n64);
    if (in == INVALID_HANDLE_VALUE) { fprintf(stderr, "cannot open file\n"); return -1; }
    if (n64 > U32MAX_) {
        /* >4 GiB needs the chunked path (prepare_big); Deflate's avail_in is
         * 32-bit. Never silently truncate here. */
        CloseHandle(in);
        fprintf(stderr, "file exceeds a single Deflate call\n");
        return -1;
    }
    size_t n = (size_t)n64;
    unsigned char *buf = scratch_ensure(&sb->in, &sb->in_cap, n + 1);
    if (!buf) { CloseHandle(in); fprintf(stderr, "oom\n"); return -1; }
    if (read_all(in, buf, n64) != 0) { CloseHandle(in); fprintf(stderr, "read err\n"); return -1; }
    CloseHandle(in);

    uint32_t crc = crc32_gzip_refl(0, buf, (size_t)n);
    size_t cap = (size_t)n + (size_t)n / 1000 + 4096;
    unsigned char *cmp = (unsigned char *)malloc(cap);
    if (!cmp) { fprintf(stderr, "oom\n"); return -1; }

    uint32_t lsize = lvl_buf_size(g_level);
    unsigned char *lb = scratch_ensure(&sb->level, &sb->level_cap, lsize);
    if (!lb) { free(cmp); fprintf(stderr, "oom\n"); return -1; }

    struct isal_zstream s;
    isal_deflate_stateless_init(&s);
    s.next_in = buf;   s.avail_in = (uint32_t)n;
    s.next_out = cmp;  s.avail_out = (uint32_t)cap;
    s.level = g_level;
    s.level_buf = lb;
    s.level_buf_size = lsize;
    s.end_of_stream = 1; s.flush = NO_FLUSH; s.gzip_flag = IGZIP_DEFLATE;
    int rc = isal_deflate_stateless(&s);
    if (rc != COMP_OK) { free(cmp); fprintf(stderr, "deflate rc=%d\n", rc); return -1; }

    uint32_t csize = s.total_out;
    if (n > 0 && (size_t)csize >= (size_t)n) {
        /* Deflate did not help (e.g. already-compressed media): store raw.
         * Hand the scratch input buffer to the entry instead of copying it. */
        free(cmp);
        e->blob = buf;
        sb->in = NULL; sb->in_cap = 0;
        e->csize = (uint64_t)n;
        e->method = 0;
    } else {
        e->blob = cmp;
        e->csize = (uint64_t)csize;
        e->method = 8;
    }
    e->crc = crc;
    e->usize = (uint64_t)n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Chunk-parallel Deflate for one large file                           */
/* ------------------------------------------------------------------ */

/* Combine reflected CRC-32 of two adjacent ranges: CRC32(a||b) from CRC32(a)
 * and CRC32(b) where len2 is the length of b. Same algorithm as zlib's
 * crc32_combine; ISA-L v2.32.1 does not export one and each chunk is CRC'd
 * in parallel, so we stitch them here. */
static uint32_t gf2_times(const uint32_t *mat, uint32_t vec) {
    uint32_t sum = 0;
    while (vec) { if (vec & 1) sum ^= *mat; vec >>= 1; mat++; }
    return sum;
}
static void gf2_square(uint32_t *sq, const uint32_t *mat) {
    for (int n = 0; n < 32; n++) sq[n] = gf2_times(mat, mat[n]);
}
static uint32_t crc32_combine(uint32_t crc1, uint32_t crc2, uint64_t len2) {
    uint32_t even[32], odd[32];
    if (len2 == 0) return crc1;
    odd[0] = 0xEDB88320u;
    { uint32_t row = 1; for (int n = 1; n < 32; n++) { odd[n] = row; row <<= 1; } }
    gf2_square(even, odd);
    gf2_square(odd, even);
    do {
        gf2_square(even, odd);
        if (len2 & 1) crc1 = gf2_times(even, crc1);
        len2 >>= 1;
        if (len2 == 0) break;
        gf2_square(odd, even);
        if (len2 & 1) crc1 = gf2_times(odd, crc1);
        len2 >>= 1;
    } while (len2 != 0);
    return crc1 ^ crc2;
}

typedef struct {
    unsigned char *in;         /* whole file (owned by the caller) */
    size_t n, chunk, nchunks;
    unsigned char **outs;      /* [nchunks] compressed chunk */
    size_t *outlens;           /* [nchunks] bytes used in outs[k] */
    uint32_t *crcs;            /* [nchunks] CRC of each input chunk */
    volatile LONG next;
    volatile LONG failed;
} chunk_job_t;

typedef struct { chunk_job_t *job; scratch_t sb; } chunk_ctx_t;

static DWORD WINAPI chunk_worker(LPVOID arg) {
    chunk_ctx_t *c = (chunk_ctx_t *)arg;
    chunk_job_t *j = c->job;
    uint32_t lsize = lvl_buf_size(g_level);
    for (;;) {
        LONG k = InterlockedIncrement(&j->next) - 1;
        if ((size_t)k >= j->nchunks) break;
        size_t off = (size_t)k * j->chunk;
        size_t len = j->n - off;
        if (len > j->chunk) len = j->chunk;
        size_t cap = len + len / 1000 + 4096;
        unsigned char *out = (unsigned char *)malloc(cap);
        unsigned char *lb = scratch_ensure(&c->sb.level, &c->sb.level_cap, lsize);
        if (!out || !lb) { free(out); j->failed = 1; return (DWORD)1; }

        struct isal_zstream s;
        isal_deflate_stateless_init(&s);
        s.next_in = j->in + off;  s.avail_in = (uint32_t)len;
        s.next_out = out;         s.avail_out = (uint32_t)cap;
        s.level = g_level;
        s.level_buf = lb;
        s.level_buf_size = lsize;
        /* FULL_FLUSH byte-aligns each chunk and resets match history, so the
         * chunks are independent and concatenate into one valid Deflate stream;
         * only the last chunk may carry BFINAL. */
        s.end_of_stream = (size_t)k == j->nchunks - 1;
        s.flush = FULL_FLUSH;
        s.gzip_flag = IGZIP_DEFLATE;
        if (isal_deflate_stateless(&s) != COMP_OK) { free(out); j->failed = 1; return (DWORD)1; }

        j->outs[k] = out;
        j->outlens[k] = s.total_out;
        j->crcs[k] = crc32_gzip_refl(0, j->in + off, (uint64_t)len);
    }
    return 0;
}

static size_t chunk_for(uint64_t n, int threads) {
    if (threads < 1) threads = 1;
    uint64_t t = n / (uint64_t)threads;
    if (t < CHUNK_MIN) t = CHUNK_MIN;
    if (t > g_chunk_max) t = g_chunk_max;
    return (size_t)t;
}

/* Should this file be split? Splitting only pays off when there are more chunks
 * than threads (otherwise file-level parallelism is better) -- except for files
 * above 4 GiB, where a single isn't representable in one uint32_t Deflate call
 * and must be chunked for correctness even on one thread. */
static int should_split(uint64_t n, int threads, size_t *chunk_out, size_t *nchunks_out) {
    if (!g_split || n < (uint64_t)CHUNK_MIN * 2) return 0;
    size_t chunk = chunk_for(n, threads);
    size_t nchunks = (size_t)((n + chunk - 1) / chunk);
    if (nchunks < 2) return 0;
    if (threads > 1 && nchunks < (size_t)threads) return 0;
    if (threads <= 1 && n <= U32MAX_) return 0;
    *chunk_out = chunk; *nchunks_out = nchunks;
    return 1;
}

/* Compress one large file in parallel chunks, then present the result as a
 * single blob on the entry (identical shape to prepare()). */
static int prepare_big(int i, int threads) {
    entry_t *e = &g_ents[i];
    uint64_t n64 = 0;
    HANDLE in = open_file_size(e->path, &n64);
    if (in == INVALID_HANDLE_VALUE) { fprintf(stderr, "cannot open file\n"); return -1; }
    size_t n = (size_t)n64;
    unsigned char *buf = (unsigned char *)malloc(n + 1);
    if (!buf) { CloseHandle(in); fprintf(stderr, "oom\n"); return -1; }
    if (read_all(in, buf, n64) != 0) { CloseHandle(in); fprintf(stderr, "read err\n"); free(buf); return -1; }
    CloseHandle(in);

    size_t chunk, nchunks;
    if (!should_split((uint64_t)n, threads, &chunk, &nchunks)) {
        /* The on-disk size changed since we stat'ed it (or the file shrank):
         * fall back to the ordinary single-stream path for this one file. */
        free(buf);
        scratch_t sb; memset(&sb, 0, sizeof(sb));
        int r = prepare(i, &sb);
        scratch_free(&sb);
        return r;
    }
    int pool = threads < (int)nchunks ? threads : (int)nchunks;
    if (pool < 1) pool = 1;

    chunk_job_t j;
    memset(&j, 0, sizeof(j));
    j.in = buf; j.n = n; j.chunk = chunk; j.nchunks = nchunks;
    j.outs = (unsigned char **)calloc(nchunks, sizeof(*j.outs));
    j.outlens = (size_t *)calloc(nchunks, sizeof(*j.outlens));
    j.crcs = (uint32_t *)calloc(nchunks, sizeof(*j.crcs));
    chunk_ctx_t *c = (chunk_ctx_t *)calloc((size_t)pool, sizeof(*c));
    HANDLE *h = (HANDLE *)malloc(sizeof(HANDLE) * (size_t)pool);
    if (!j.outs || !j.outlens || !j.crcs || !c || !h) {
        fprintf(stderr, "oom\n");
        free(j.outs); free(j.outlens); free(j.crcs); free(c); free(h); free(buf);
        return -1;
    }
    for (int t = 0; t < pool; t++) { c[t].job = &j; h[t] = CreateThread(NULL, 0, chunk_worker, &c[t], 0, NULL); }
    WaitForMultipleObjects((DWORD)pool, h, TRUE, INFINITE);
    for (int t = 0; t < pool; t++) { CloseHandle(h[t]); scratch_free(&c[t].sb); }
    free(c); free(h);

    if (j.failed) { for (size_t k = 0; k < nchunks; k++) free(j.outs[k]); free(j.outs); free(j.outlens); free(j.crcs); free(buf); return -1; }

    uint64_t total = 0;
    for (size_t k = 0; k < nchunks; k++) total += j.outlens[k];
    uint32_t crc = 0;
    for (size_t k = 0; k < nchunks; k++) {
        size_t off = k * chunk, len = n - off;
        if (len > chunk) len = chunk;
        crc = (k == 0) ? j.crcs[0] : crc32_combine(crc, j.crcs[k], (uint64_t)len);
    }

    if (n > 0 && total >= (uint64_t)n) {
        for (size_t k = 0; k < nchunks; k++) free(j.outs[k]);
        e->blob = buf;                    /* store raw, keep the input buffer */
        e->csize = (uint64_t)n;
        e->method = 0;
    } else {
        unsigned char *blob = (unsigned char *)malloc(total ? (size_t)total : 1);
        if (!blob) { fprintf(stderr, "oom\n"); for (size_t k = 0; k < nchunks; k++) free(j.outs[k]); free(j.outs); free(j.outlens); free(j.crcs); free(buf); return -1; }
        size_t o = 0;
        for (size_t k = 0; k < nchunks; k++) { memcpy(blob + o, j.outs[k], j.outlens[k]); o += j.outlens[k]; free(j.outs[k]); }
        free(buf);
        e->blob = blob;
        e->csize = total;
        e->method = 8;
    }
    free(j.outs); free(j.outlens); free(j.crcs);
    e->crc = crc;
    e->usize = (uint64_t)n;
    return 0;
}

/* One worker of the file-level pool: it owns a scratch and pulls entries off a
 * shared index list (todo[]) so per-file sizes spread across the threads. */
typedef struct {
    scratch_t sb;
    const int *todo;
    int ntodo;
    int failed;
} worker_ctx_t;

static DWORD WINAPI worker(LPVOID arg) {
    worker_ctx_t *w = (worker_ctx_t *)arg;
    for (;;) {
        LONG k = InterlockedIncrement(&g_next) - 1;
        if (k >= w->ntodo) break;
        if (prepare(w->todo[k], &w->sb) != 0) { w->failed = 1; return (DWORD)1; }
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
    int need64 = g_force64 || (unsigned)g_nfiles >= U16MAX_ ||
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

    /* Tunables: ISAL_ZIP_CHUNK caps the split chunk (bytes); ISAL_ZIP_SPLIT=0
     * turns the single-file parallel path off entirely (for A/B benchmarks). */
    g_chunk_max = CHUNK_MAX_DEFAULT;
    const char *cenv = getenv("ISAL_ZIP_CHUNK");
    if (cenv) { long v = atol(cenv); if (v >= (long)CHUNK_MIN) g_chunk_max = (size_t)v; else if (v > 0) g_chunk_max = CHUNK_MIN; }
    const char *senv = getenv("ISAL_ZIP_SPLIT");
    g_split = !(senv && senv[0] == '0');

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

    /* The output path is UTF-8 like every other path here; go wide so it does
     * not depend on the CRT's locale/ANSI code page (and so we never touch the
     * host process's locale). */
    wchar_t *outw = u8_to_wide(outpath);
    if (!outw) { fprintf(stderr, "oom\n"); return 1; }
    FILE *out = _wfopen(outw, L"wb");
    free(outw);
    if (!out) { fprintf(stderr, "cannot open output: %s\n", outpath); return 1; }
    /* An OOXML package is thousands of tiny header/blob writes; a bigger buffer
     * keeps them out of the kernel. */
    setvbuf(out, NULL, _IOFBF, 1u << 20);

    double t0 = now();
    int rc = 0;

    /* Phase A: files large enough (and numerous enough in chunks) to be split
     * across the whole thread pool. Doing them one at a time spends all cores
     * on a single stream, which is the only way to speed up one big file. */
    for (int i = 0; i < g_nfiles && rc == 0; i++) {
        entry_t *e = &g_ents[i];
        if (!e->path) continue;
        size_t ch, nc;
        if (should_split(e->fsize, threads, &ch, &nc)) {
            (void)ch; (void)nc;
            if (prepare_big(i, threads) != 0) { rc = 1; break; }
            e->done = 1;
        }
    }

    /* Phase B: everything else, one file per worker. */
    int *todo = (int *)malloc(sizeof(int) * (g_nfiles > 0 ? g_nfiles : 1));
    if (!todo) { fprintf(stderr, "oom\n"); rc = 1; }
    int nt = 0;
    if (!rc) {
        for (int i = 0; i < g_nfiles; i++) if (!g_ents[i].done) todo[nt++] = i;
    }
    int pool = threads < nt ? threads : nt;
    if (!rc && nt > 0 && pool <= 1) {
        scratch_t sb; memset(&sb, 0, sizeof(sb));
        for (int k = 0; k < nt; k++) if (prepare(todo[k], &sb) != 0) { rc = 1; break; }
        scratch_free(&sb);
    } else if (!rc && nt > 0) {
        worker_ctx_t *w = (worker_ctx_t *)calloc((size_t)pool, sizeof(*w));
        HANDLE *h = (HANDLE *)malloc(sizeof(HANDLE) * (size_t)pool);
        if (!w || !h) { fprintf(stderr, "oom\n"); rc = 1; }
        else {
            g_next = 0;
            for (int t = 0; t < pool; t++) {
                w[t].todo = todo; w[t].ntodo = nt;
                h[t] = CreateThread(NULL, 0, worker, &w[t], 0, NULL);
            }
            WaitForMultipleObjects((DWORD)pool, h, TRUE, INFINITE);
            for (int t = 0; t < pool; t++) { CloseHandle(h[t]); if (w[t].failed) rc = 1; }
            for (int t = 0; t < pool; t++) scratch_free(&w[t].sb);
        }
        free(w); free(h);
    }
    free(todo);

    if (rc == 0) rc = write_archive(out);
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

#ifdef ISAL_ZIP_DLL
/* Pin this module in the host process once. SAS's MODULE loader loads the DLL
 * per DATA step and calls FreeLibrary when the step ends; pinning it
 * (GET_MODULE_HANDLE_EX_FLAG_PIN) makes the unload a no-op, so the DLL -- and
 * its isa-l.dll dependency, and its loaded state -- stay resident instead of
 * being re-loaded/re-resolved for every step that calls us. */
static void pin_module_once(void) {
    static LONG pinned = 0;
    if (InterlockedCompareExchange(&pinned, 1, 0) == 0) {
        HMODULE h = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)(const void *)&pin_module_once, &h);
    }
}
#else
#define pin_module_once() ((void)0)
#endif

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
    pin_module_once();
    if (!dir || !dir[0]) return 2;
    wchar_t *w = u8_to_wide(dir);
    if (!w) return 3;
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
    pin_module_once();
    if (!outZip || !rootDir || !outZip[0] || !rootDir[0]) return 2;
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
