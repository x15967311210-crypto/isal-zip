# isal-zip

A small Windows tool that builds **standard `.zip` archives** — including **OOXML**
(`.docx` / `.xlsx` / `.pptx`) — using **Intel ISA-L**'s high-performance Deflate
engine, plus the benchmark harness used to compare it against `7z.exe`.

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

`isal_zip` links Intel ISA-L, so ISA-L must be built first. On Windows with
NASM + MinGW-w64 gcc + CMake + `mingw32-make` + git on `PATH`:

```powershell
.\build.ps1
```

The script clones Intel ISA-L (default `v2.32.1`), builds `isa-l.dll`, then
compiles `src\isal_zip.c` against it. Use `-IsaLSrc C:\src\isa-l` to reuse an
existing checkout.

## Usage

```
isal_zip <level> <out.zip> [--root] <file-or-dir1> [<file-or-dir2> ...]

  level          ISA-L compression level: 0=store, 1=fastest, 2, 3=best
  --root         store a directory's *contents* at the archive root
                 (required for OOXML/OPC packages)
  env ISAL_ZIP_THREADS=N   compress N files in parallel (default 1)
```

Examples:

```powershell
# package a folder (keeps the top-level folder name)
.\isal_zip.exe 1 out.zip .\some_folder

# package an unpacked OOXML package (contents at the root) -> openable .docx
.\isal_zip.exe 1 out.docx --root .\unpacked_docx
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
Verified by round-tripping real Word documents (including one with an embedded
image) through `isal_zip --root` and reopening them in Word.

Current limitations: no ZIP64 (> 4 GB / > 65535 entries), no explicit directory
entries, no UTF-8 name flag, no encryption.

## Repository layout

```
src/isal_zip.c      ZIP writer built on Intel ISA-L's isal_deflate_stateless()
build.ps1           builds isa-l.dll + isal_zip.exe
bench/bench_isal.py ISA-L (python-isal) vs 7z vs zlib, single stream
bench/bench_zip.py  isal_zip.exe vs 7z, file or folder (end-to-end)
```

## License

BSD-3-Clause. Intel ISA-L itself is BSD-3-Clause and is **not** vendored here;
fetch it via `build.ps1` / <https://github.com/intel/isa-l>.
