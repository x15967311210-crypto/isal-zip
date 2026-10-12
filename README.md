# isal-zip

[![ci](https://github.com/x15967311210-crypto/isal-zip/actions/workflows/ci.yml/badge.svg)](https://github.com/x15967311210-crypto/isal-zip/actions/workflows/ci.yml)

A small Windows tool that builds **standard `.zip` archives** — including **OOXML**
(`.docx` / `.xlsx` / `.pptx`) — using **Intel ISA-L**'s high-performance Deflate
engine, plus the benchmark harness used to compare it against `7z.exe`.

> 中文说明见 [README.zh-CN.md](README.zh-CN.md).

Intel ISA-L (`igzip`) is a *compression library*: it produces raw Deflate/gzip
streams but knows nothing about the ZIP container. This repo wraps it into a
minimal, spec-conformant ZIP writer and measures how it stacks up against
7-Zip's zip compressor.

## TL;DR

* ISA-L Deflate is **~10× faster** than `7z`'s fastest zip level (single stream).
* Wrapped into a real `.zip` writer, `isal_zip` is still **~7× faster** than
  `7z -mx1` on a single file, and **~5.6×** on a folder (8 threads) — 7z only
  closes the gap by compressing multiple files in parallel.
* 7-Zip wins on **size**: even its fastest `-mx1` produces ~**9–10 % smaller**
  output, and `-mx5..-mx9` are *orders of magnitude* slower (multi-pass Deflate).

## Features

* Deflate via Intel ISA-L (`isal_deflate_stateless`), CRC-32 via
  `crc32_gzip_refl`.
* Recursive folder packaging; multiple threads (`ISAL_ZIP_THREADS`).
* **ZIP64** — parts or archives larger than 4 GiB / >65535 entries.
* **UTF-8 filename flag** (general-purpose bit 11) for non-ASCII names.
* Explicit **directory entries**, and a **store fallback** when Deflate does not
  shrink a part (e.g. already-compressed media).
* **OOXML/OPC safe**: `--root` puts a folder's contents at the archive root and
  `[Content_Types].xml` is forced to be the first entry.

Prebuilt `isal_zip.exe` + `isal_zip.dll` + `isa-l.dll` are attached to each
[release](https://github.com/x15967311210-crypto/isal-zip/releases).

## Benchmark results

Environment: Intel Core i7-13650HX (20 threads), 16 GB RAM, Windows ·
7-Zip 23.01 x64 · Intel ISA-L 2.32.1 · `python-isal` 1.8.0.
Datasets: `enwik8` (100 MB text) and the concatenated Silesia corpus (212 MB).
Best of N runs, single stream / single thread unless noted.

### Deflate core: ISA-L vs 7z vs zlib (MB/s, compression ratio)

**enwik8 (100 MB)**

| Compressor | Level | Speed | Ratio |
| --- | --- | ---: | ---: |
| ISA-L | 1 | 274 MB/s | 2.40 |
| ISA-L | 3 | 154 MB/s | 2.53 |
| 7z zip | -mx1 | 26.8 MB/s | 2.64 |
| 7z zip | -mx5 | 5.9 MB/s | 2.84 |
| 7z zip | -mx9 | 1.10 MB/s | 2.85 |
| zlib | 1 / 6 / 9 | 124 / 34.1 / 18.6 | 1.90 / 2.71 / 2.74 |

**Silesia (212 MB)**

| Compressor | Level | Speed | Ratio |
| --- | --- | ---: | ---: |
| ISA-L | 1 | 382 MB/s | 2.71 |
| ISA-L | 3 | 180 MB/s | 2.82 |
| 7z zip | -mx1 | 38.2 MB/s | 3.02 |
| 7z zip | -mx5 | 6.9 MB/s | 3.23 |
| 7z zip | -mx9 | 0.75 MB/s | 3.28 |
| zlib | 1 / 6 / 9 | 152 / 44.5 / 20.0 | 2.25 / 3.08 / 3.14 |

Speed-ups: ISA-L lvl1 / 7z `-mx1` ≈ **10×**; ISA-L lvl3 / 7z `-mx5` ≈ **26×**;
ISA-L lvl3 / 7z `-mx9` ≈ **139–242×**.

### With the ZIP wrapper: `isal_zip` vs `7z` (end-to-end, real `.zip`)

**Single file, enwik8 (100 MB)** — fastest level each

| Tool | Level | Threads | Time | Speed | Ratio |
| --- | --- | --- | ---: | ---: | ---: |
| isal_zip | 1 | 1 | 477 ms | 210 MB/s | 2.40 |
| 7z zip | -mx1 | default | 3506 ms | 28.5 MB/s | 2.64 |

→ isal_zip **7.35× faster**, 7z **9.2 % smaller**.

**Folder, Silesia (12 files, 212 MB)** — fastest level each

| Tool | Level | Threads | Time | Speed | Ratio |
| --- | --- | --- | ---: | ---: | ---: |
| isal_zip | 1 | 1 | 752 ms | 282 MB/s | 2.71 |
| isal_zip | 1 | 8 | 250 ms | 846 MB/s | 2.71 |
| 7z zip | -mx1 | default | 1395 ms | 152 MB/s | 3.02 |

→ isal_zip **1.9× (1 thread) / 5.6× (8 threads) faster**, 7z **10.1 % smaller**.
(7-Zip's folder case already uses multiple threads across files; `isal_zip`
scales the same way via `ISAL_ZIP_THREADS`.)

## Build

`isal_zip` links Intel ISA-L, so ISA-L must be built first. On Windows:

```powershell
.\build.ps1                 # MinGW Makefiles + NASM + MinGW gcc on PATH
.\build.ps1 -Generator Ninja -Gcc C:\msys64\ucrt64\bin\gcc.exe
```

From an MSYS2 / MinGW shell:

```bash
./build.sh                  # clones ISA-L v2.32.1; builds isa-l.dll + isal_zip.exe + isal_zip.dll
```

Both scripts clone Intel ISA-L (default `v2.32.1`), build `isa-l.dll`, then
compile `src\isal_zip.c` twice against it: once as `isal_zip.exe`, and once with
`-DISAL_ZIP_DLL` as `isal_zip.dll` (which exports `isal_zip_pack`). Use
`-IsaLSrc` / `ISAL_SRC` to reuse an existing checkout. CI
(`.github/workflows/ci.yml`) builds and smoke-tests both on `windows-latest`.

## Usage

```
isal_zip <level> <out.zip> [--root] <file-or-dir1> [<file-or-dir2> ...]

  level          ISA-L compression level: 0=store, 1=fastest, 2, 3=best
  --root         store a directory's *contents* at the archive root
                 (required for OOXML/OPC packages)
  env ISAL_ZIP_THREADS=N   compress N files in parallel (default 1)
  env ISAL_ZIP_FORCE64=1   always emit ZIP64 records (for testing)
```

Examples:

```powershell
# package a folder (keeps the top-level folder name)
.\isal_zip.exe 1 out.zip .\some_folder

# package an unpacked OOXML package (contents at the root) -> openable .docx
.\isal_zip.exe 1 out.docx --root .\unpacked_docx
```

## Using the DLL (`isal_zip.dll`)

The same ZIP writer is exported as a shared library so a host process can pack
directly — no shelling out to `isal_zip.exe`. Two functions are exported:

```c
int isal_zip_pack(const char *outZip, const char *rootDir, int level, int threads);
int isal_zip_rmtree(const char *dir);
```

| arg | meaning |
| --- | --- |
| `outZip` | full path of the `.zip` to write |
| `rootDir` | source folder; its **contents** go to the archive root (like `--root`) |
| `level` | ISA-L level `0..3` |
| `threads` | parallel worker threads (`<= 1` means single-threaded) |

Returns `0` on success, non-zero on failure (`2` for a null/empty path argument).
Because the source folder is packed *at the archive root*, the OPC first-entry
rule applies: `[Content_Types].xml`, if present, is forced to be the first entry.

`isal_zip_rmtree(dir)` recursively deletes a directory tree from inside the host
process — no `cmd.exe`, `rd` or hidden console window. It takes a UTF-8 path,
clears read-only attributes, never follows reparse points/junctions, and
returns `0` on success **including when the path does not exist**, so it can be
used as an idempotent "clean before use" step. Non-zero (`2` for null/empty)
means something could not be removed.

Both exports pin the module on first use (`GetModuleHandleEx` +
`GET_MODULE_HANDLE_EX_FLAG_PIN`). Hosts that load the DLL per call — SAS's
`MODULE(…)`/`MODULEN(…)` loads it for each DATA step and unloads it when the
step ends — therefore keep it (and `isa-l.dll`) resident: measured ~1.5 ms of
load/unload saved per DATA step. The flip side of pinning: while a host process
holds the DLL, the file cannot be replaced on disk.

`isal_zip.dll` and `isa-l.dll` must be on the DLL search path (e.g. next to the
host executable, or a directory on `PATH`). `ISAL_ZIP_FORCE64=1` still forces
ZIP64 records; set `ISAL_ZIP_VERBOSE=1` to get the one-line summary that the exe
prints (silent by default so it does not pollute a host's output).

Calling it from Python:

```python
import ctypes
lib = ctypes.CDLL(r"C:\path\to\isal_zip.dll")
lib.isal_zip_pack.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
lib.isal_zip_pack.restype = ctypes.c_int
rc = lib.isal_zip_pack(b"out.docx", b"unpacked_docx", 1, 8)   # 0 == ok
```

Cleaning the scratch folder afterwards:

```python
lib.isal_zip_rmtree.argtypes = [ctypes.c_char_p]
rc = lib.isal_zip_rmtree(b"unpacked_docx")                    # 0 == ok (even if gone)
```

Calling it from SAS (SASCBTBL / `modulen`):

```sas
filename sascbtbl "your\sascbtbl.dsc";
data _null_;
  file sascbtbl;
  put "routine isal_zip_pack minarg=4 maxarg=4 module=isal_zip stackpop=called returns=long;";
  put " arg 1 char        input format=$CSTR1024.;";
  put " arg 2 char        input format=$CSTR1024.;";
  put " arg 3 num byvalue input format=PIB4.      ;";
  put " arg 4 num byvalue input format=PIB4.      ;";
run;
```

## OOXML / OPC notes

`.docx`/`.xlsx`/`.pptx` are ZIP files subject to the Open Packaging
Conventions. Two rules matter for the zip writer:

1. Part names are **package-root-relative** — a top-level folder is *not*
   allowed, hence `--root`.
2. The **`[Content_Types].xml`** stream must be the **first** entry in the
   archive. `isal_zip` enforces this.

Everything else that 7-Zip/Word do (explicit directory entries, storing the
content-types stream uncompressed) is *optional* — modern Office reads a ZIP
where every part is Deflate-compressed and `[Content_Types].xml` is first.
`isal_zip` writes directory entries too and stores parts that Deflate cannot
shrink. Verified by round-tripping real Word documents (including one with an
embedded image) through `isal_zip --root` and reopening them in Word.

Not implemented: encryption, data descriptors. (Neither is needed for OOXML.)

## Repository layout

```
src/isal_zip.c        ZIP writer built on Intel ISA-L's isal_deflate_stateless()
                      (compiled as both isal_zip.exe and isal_zip.dll)
build.ps1 / build.sh  build isa-l.dll + isal_zip.exe + isal_zip.dll (PowerShell / MSYS2)
.github/workflows/    CI: build + smoke test (exe and dll) on windows-latest
bench/bench_isal.py   ISA-L (python-isal) vs 7z vs zlib, single stream
bench/bench_zip.py    isal_zip.exe vs 7z, file or folder (end-to-end)
```

## License

BSD-3-Clause. Intel ISA-L itself is BSD-3-Clause and is **not** vendored here;
it is fetched by the build scripts / <https://github.com/intel/isa-l>.
