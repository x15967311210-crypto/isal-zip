#!/usr/bin/env python3
"""isal_zip.exe vs 7z.exe -- both build a real .zip from a file or a folder.

For a folder both tools recurse it, so this is the apples-to-apples
"package a directory" comparison. isal_zip uses --root so the directory
contents land at the archive root (required for OOXML/OPC).

Usage:
    python bench_zip.py --in some_dir_or_file [--level 1] [--threads 1,8]
        [--isal-zip .\\isal_zip.exe] [--7z "C:\\Program Files\\7-Zip\\7z.exe"] [--runs 3]
        [--split 0|1] [--chunk BYTES]
"""
import argparse
import os
import subprocess
import time

MB = 1_000_000.0
SEVENZIP_DEFAULT = os.environ.get("SEVENZIP", r"C:\Program Files\7-Zip\7z.exe")


def time_cmd(cmd, out, runs, env=None):
    bt = None
    for _ in range(runs):
        if os.path.exists(out):
            os.remove(out)
        t0 = time.perf_counter()
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        dt = time.perf_counter() - t0
        if r.returncode != 0:
            raise RuntimeError(f"command failed rc={r.returncode}: {cmd}")
        bt = dt if bt is None else min(bt, dt)
    return bt, os.path.getsize(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--in", dest="inp", required=True, help="file or folder to package")
    ap.add_argument("--isal-zip", default=os.path.join(".", "isal_zip.exe"))
    ap.add_argument("--7z", dest="sevenzip", default=SEVENZIP_DEFAULT)
    ap.add_argument("--level", type=int, default=1, help="compression level (fastest=1)")
    ap.add_argument("--threads", default="1", help="comma list, e.g. 1,8")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--workdir", default=os.getcwd())
    ap.add_argument("--split", default="",
                    help="set ISAL_ZIP_SPLIT (0 disables single-file splitting)")
    ap.add_argument("--chunk", default="",
                    help="set ISAL_ZIP_CHUNK (bytes) for split chunks")
    args = ap.parse_args()

    inp = os.path.abspath(args.inp)
    isdir = os.path.isdir(inp)
    if isdir:
        total = sum(os.path.getsize(os.path.join(dp, f))
                    for dp, _, fs in os.walk(inp) for f in fs)
    else:
        total = os.path.getsize(inp)
    print(f"input : {inp}  ({total/MB:.1f} MB, {'folder' if isdir else 'file'})")
    print(f"level : {args.level}   runs: {args.runs} (best)\n")

    outz = os.path.join(args.workdir, "_cmp.zip")
    for th in str(args.threads).split(","):
        env = dict(os.environ)
        env["ISAL_ZIP_THREADS"] = th
        if args.split != "":
            env["ISAL_ZIP_SPLIT"] = args.split
        if args.chunk != "":
            env["ISAL_ZIP_CHUNK"] = args.chunk
        if isdir:
            cmd = [args.isal_zip, str(args.level), outz, "--root", inp]
        else:
            cmd = [args.isal_zip, str(args.level), outz, inp]
        t, sz = time_cmd(cmd, outz, args.runs, env)
        tag = f" ({th} th" + (f", split={args.split}" if args.split != "" else "") + ")"
        print(f"isal_zip lvl{args.level}{tag:18s}: {t*1000:8.1f} ms  {total/MB/t:8.1f} MB/s  ratio {total/sz:5.2f}")

    t, sz = time_cmd([args.sevenzip, "a", "-tzip", "-mm=Deflate", f"-mx={args.level}",
                      "-y", "-bso0", "-bsp0", outz, inp], outz, args.runs)
    print(f"7z       -mx{args.level}        : {t*1000:8.1f} ms  {total/MB/t:8.1f} MB/s  ratio {total/sz:5.2f}")


if __name__ == "__main__":
    main()
