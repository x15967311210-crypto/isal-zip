#!/usr/bin/env python3
"""Intel ISA-L Deflate vs 7z.exe (zip / Deflate) vs zlib -- single-stream throughput.

ISA-L is measured in-process through `python-isal` (the same igzip engine that
isal_zip links). 7z.exe is measured end-to-end (it reads the file, compresses,
and writes a .zip).

Usage:
    python bench_isal.py --data corpus.bin [--7z "C:\\Program Files\\7-Zip\\7z.exe"] [--runs 3]

Install ISA-L binding first:
    pip install isal
"""
import argparse
import os
import subprocess
import time
import zlib

try:
    import isal.isal_zlib as isalz
except ImportError:
    isalz = None

MB = 1_000_000.0
SEVENZIP_DEFAULT = os.environ.get("SEVENZIP", r"C:\Program Files\7-Zip\7z.exe")


def isal_raw(data, level):
    co = isalz.compressobj(level, isalz.DEFLATED, -15)  # raw deflate
    return co.compress(data) + co.flush()


def zlib_raw(data, level):
    return zlib.compress(data, level, wbits=-15)


def best(fn, runs):
    bt = None
    out = None
    for _ in range(runs):
        t0 = time.perf_counter()
        out = fn()
        dt = time.perf_counter() - t0
        bt = dt if bt is None else min(bt, dt)
    return bt, out


def sevenzip_zip(sevenzip, infile, level, outpath, runs):
    bt = None
    for _ in range(runs):
        if os.path.exists(outpath):
            os.remove(outpath)
        cmd = [sevenzip, "a", "-tzip", "-mm=Deflate", f"-mx={level}", "-y",
               "-bso0", "-bsp0", outpath, infile]
        t0 = time.perf_counter()
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dt = time.perf_counter() - t0
        if r.returncode != 0:
            raise RuntimeError(f"7z failed rc={r.returncode}")
        bt = dt if bt is None else min(bt, dt)
    return bt, os.path.getsize(outpath)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", required=True, help="input file to compress")
    ap.add_argument("--7z", dest="sevenzip", default=SEVENZIP_DEFAULT)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--workdir", default=os.getcwd())
    args = ap.parse_args()

    size = os.path.getsize(args.data)
    with open(args.data, "rb") as f:
        data = f.read()
    print(f"dataset : {args.data}  ({size / MB:.1f} MB)")
    print(f"runs    : {args.runs} (best)\n")

    if isalz is not None:
        for lvl in (1, 2, 3):
            t, out = best(lambda lvl=lvl: isal_raw(data, lvl), args.runs)
            print(f"ISA-L   lvl{lvl} : {t*1000:8.1f} ms  {size/MB/t:8.1f} MB/s  ratio {size/len(out):5.2f}")
    else:
        print("(python-isal not installed -- `pip install isal` to measure ISA-L)")

    for lvl in (1, 6, 9):
        t, out = best(lambda lvl=lvl: zlib_raw(data, lvl), args.runs)
        print(f"zlib    lvl{lvl} : {t*1000:8.1f} ms  {size/MB/t:8.1f} MB/s  ratio {size/len(out):5.2f}")

    out_zip = os.path.join(args.workdir, "_bench_single.zip")
    for lvl in (1, 3, 5, 7, 9):
        t, sz = sevenzip_zip(args.sevenzip, args.data, lvl, out_zip, args.runs)
        print(f"7z zip  mx{lvl} : {t*1000:8.1f} ms  {size/MB/t:8.1f} MB/s  ratio {size/sz:5.2f}")


if __name__ == "__main__":
    main()
