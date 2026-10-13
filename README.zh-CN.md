# isal-zip

[![ci](https://github.com/x15967311210-crypto/isal-zip/actions/workflows/ci.yml/badge.svg)](https://github.com/x15967311210-crypto/isal-zip/actions/workflows/ci.yml)

一个 Windows 小工具：用 **Intel ISA-L** 的高性能 Deflate 引擎生成**标准 `.zip` 归档**，
包括 **OOXML**（`.docx` / `.xlsx` / `.pptx`），并附带用来对比 `7z.exe` 的基准测试脚本。

> English: [README.md](README.md)

Intel ISA-L（`igzip`）是一个**压缩库**——它只产原始 Deflate/gzip 流，完全不管 ZIP 容器。
本仓库把它包成一个符合规范的最小 ZIP 写入器，并量化它与 7-Zip 的 zip 压缩器的差距。

## 结论速览

* 单流下 ISA-L 的 Deflate 比 `7z` 最快档快 **约 10×**。
* 做成真正的 `.zip` 后，`isal_zip` 在单文件上仍比 `7z -mx1` 快 **约 7×**，在文件夹上（8 线程）快 **约 5.6×**——7z 只是靠跨文件并行才追近一点。
* 7z 赢在**体积**：即便最快的 `-mx1` 也比 ISA-L 小 **约 9–10%**，而 `-mx5..-mx9` 会慢上**几个数量级**（多趟 Deflate）。

## 功能

* 用 Intel ISA-L 的 `isal_deflate_stateless` 做 Deflate，CRC-32 用 `crc32_gzip_refl`。
* 全程使用 Win32 文件 I/O——`CreateFileW`/`ReadFile`（每个文件一次读取，不经 CRT stdio 的
  4 KiB 中转缓冲），遍历用 `FindFirstFileW`（一次调用同时拿到属性和 64 位大小，省掉单独的 `stat`）。
* 目录递归打包；多线程（`ISAL_ZIP_THREADS`）——既能跨文件并行，也能在**单个大文件内部**并行：
  大文件被切成互相独立的 Deflate 块并行压缩后拼接。
* **ZIP64**——支持超过 4 GiB 的部件或归档、超过 65535 个条目。
* **UTF-8 文件名标志位**（通用位 11），自动为非 ASCII 名称置位。
* 写显式**目录条目**；当 Deflate 压不小某个部件时**回退为存储**（如已压缩的媒体）。
* **对 OOXML/OPC 安全**：`--root` 把目录内容放到归档根，并强制 `[Content_Types].xml` 为第一个条目。

每次 [Release](https://github.com/x15967311210-crypto/isal-zip/releases) 都附带预编译的
`isal_zip.exe` + `isal_zip.dll` + `isa-l.dll`。

## 基准结果

环境：Intel Core i7-13650HX（20 线程）、16 GB 内存、Windows ·
7-Zip 23.01 x64 · Intel ISA-L 2.32.1 · `python-isal` 1.8.0。
数据：`enwik8`（100 MB 文本）与拼接后的 Silesia 语料（212 MB）。
取多次最优值；除注明外均为单流/单线程。

### Deflate 内核：ISA-L vs 7z vs zlib（MB/s，压缩率）

**enwik8（100 MB）**

| 压缩器 | 档位 | 速度 | 压缩率 |
| --- | --- | ---: | ---: |
| ISA-L | 1 | 274 MB/s | 2.40 |
| ISA-L | 3 | 154 MB/s | 2.53 |
| 7z zip | -mx1 | 26.8 MB/s | 2.64 |
| 7z zip | -mx5 | 5.9 MB/s | 2.84 |
| 7z zip | -mx9 | 1.10 MB/s | 2.85 |
| zlib | 1 / 6 / 9 | 124 / 34.1 / 18.6 | 1.90 / 2.71 / 2.74 |

**Silesia（212 MB）**

| 压缩器 | 档位 | 速度 | 压缩率 |
| --- | --- | ---: | ---: |
| ISA-L | 1 | 382 MB/s | 2.71 |
| ISA-L | 3 | 180 MB/s | 2.82 |
| 7z zip | -mx1 | 38.2 MB/s | 3.02 |
| 7z zip | -mx5 | 6.9 MB/s | 3.23 |
| 7z zip | -mx9 | 0.75 MB/s | 3.28 |
| zlib | 1 / 6 / 9 | 152 / 44.5 / 20.0 | 2.25 / 3.08 / 3.14 |

提速：ISA-L lvl1 / 7z `-mx1` ≈ **10×**；ISA-L lvl3 / 7z `-mx5` ≈ **26×**；
ISA-L lvl3 / 7z `-mx9` ≈ **139–242×**。

### 包上 ZIP 容器后：`isal_zip` vs `7z`（端到端，真实 `.zip`）

**单文件 enwik8（100 MB）**——双方各自最快档

| 工具 | 档位 | 线程 | 用时 | 速度 | 压缩率 |
| --- | --- | --- | ---: | ---: | ---: |
| isal_zip | 1 | 1 | 477 ms | 210 MB/s | 2.40 |
| 7z zip | -mx1 | 默认 | 3506 ms | 28.5 MB/s | 2.64 |

→ isal_zip **快 7.35×**，7z **小 9.2%**。

**文件夹 Silesia（12 文件，212 MB）**——双方各自最快档

| 工具 | 档位 | 线程 | 用时 | 速度 | 压缩率 |
| --- | --- | --- | ---: | ---: | ---: |
| isal_zip | 1 | 1 | 752 ms | 282 MB/s | 2.71 |
| isal_zip | 1 | 8 | 250 ms | 846 MB/s | 2.71 |
| 7z zip | -mx1 | 默认 | 1395 ms | 152 MB/s | 3.02 |

→ isal_zip **快 1.9×（单线程）/ 5.6×（8 线程）**，7z **小 10.1%**。
（7-Zip 在文件夹场景本就跨文件多线程；`isal_zip` 通过 `ISAL_ZIP_THREADS` 同样可扩展。）

## 构建

`isal_zip` 依赖 Intel ISA-L，所以要先编译 ISA-L。在 Windows 上：

```powershell
.\build.ps1                 # 需要 PATH 中有 MinGW Makefiles + NASM + MinGW gcc
.\build.ps1 -Generator Ninja -Gcc C:\msys64\ucrt64\bin\gcc.exe
```

在 MSYS2 / MinGW shell 中：

```bash
./build.sh                  # 克隆 ISA-L v2.32.1，编译 dll + exe + isal_zip.dll
```

两个脚本都会克隆 Intel ISA-L（默认 `v2.32.1`）、编译 `isa-l.dll`，再把
`src\isal_zip.c` 编两遍：一遍是 `isal_zip.exe`，一遍加 `-DISAL_ZIP_DLL` 编成
`isal_zip.dll`（导出 `isal_zip_pack`）。用 `-IsaLSrc` / `ISAL_SRC` 复用已有源码目录。
CI（`.github/workflows/ci.yml`）会在 `windows-latest` 上构建并跑 exe 与 dll 的 smoke test。

## 用法

```
isal_zip <level> <out.zip> [--root] <文件或目录1> [<文件或目录2> ...]

  level          ISA-L 压缩档位：0=store，1=最快，2，3=最优
  --root         把目录的“内容”放到归档根（OOXML/OPC 必需）
  环境变量 ISAL_ZIP_THREADS=N   N 个 worker：并行压缩 N 个文件；单个大文件切成 N 块并行（默认 1）
  环境变量 ISAL_ZIP_CHUNK=字节  切分大文件时每块大小的上限（默认 4 MiB，速度与压缩率折中）
  环境变量 ISAL_ZIP_SPLIT=0     关闭单文件切分（只用文件级并行）
  环境变量 ISAL_ZIP_FORCE64=1   始终输出 ZIP64 记录（用于测试）
```

示例：

```powershell
# 打包一个文件夹（保留顶层文件夹名）
.\isal_zip.exe 1 out.zip .\some_folder

# 打包解包后的 OOXML 包（内容放到根）-> 可正常打开的 .docx
.\isal_zip.exe 1 out.docx --root .\unpacked_docx
```

## 用 DLL 直接导出 zip（`isal_zip.dll`）

同一个 ZIP 写入器也导出成动态库，宿主进程可以直接打包，不必再去调用 `isal_zip.exe`。
导出两个函数：

```c
int isal_zip_pack(const char *outZip, const char *rootDir, int level, int threads);
int isal_zip_rmtree(const char *dir);
```

| 参数 | 含义 |
| --- | --- |
| `outZip` | 输出 `.zip` 全路径 |
| `rootDir` | 源目录；其**内容**放到归档根（等价于 `--root`） |
| `level` | ISA-L 档位 `0..3` |
| `threads` | 并行线程数（`<= 1` 为单线程） |

成功返回 `0`，失败返回非 0（路径参数为空返回 `2`）。因为源目录直接放在归档根，
同样遵守 OPC 规则：若存在 `[Content_Types].xml`，会强制它成为第一个条目。

`isal_zip_rmtree(dir)` 在**宿主进程内**递归删除一棵目录树——不需要 `cmd.exe`、
`rd`，也不会弹窗口。参数是 UTF-8 路径；会清掉只读属性、不跟进 reparse point
（junction/符号链接只删链接本身）；**路径不存在也算成功返回 0**，所以可以放心
当"用前先清"的幂等步骤。非 0（空参数返回 `2`）表示有东西没删掉。

两个导出函数在**首次调用时会把本模块钉住**（`GetModuleHandleEx` +
`GET_MODULE_HANDLE_EX_FLAG_PIN`）。对于"每次调用都加载/卸载 DLL"的宿主——SAS 的
`MODULE(…)`/`MODULEN(…)` 就是每个 DATA 步加载、步末卸载——钉住后 DLL（连同
`isa-l.dll`）会常驻：实测每步省掉约 1.5 ms 的加载/卸载开销。代价是宿主进程还活着时，
磁盘上的 DLL 文件无法被覆盖。

`isal_zip.dll` 与 `isa-l.dll` 需在 DLL 搜索路径上（放在宿主程序旁边，或某个 `PATH`
目录里）。`ISAL_ZIP_FORCE64=1` 仍可强制 ZIP64；`ISAL_ZIP_VERBOSE=1` 可让 DLL 也
打印 exe 那行统计（默认静默，避免污染宿主输出）。

Python 调用示例：

```python
import ctypes
lib = ctypes.CDLL(r"C:\path\to\isal_zip.dll")
lib.isal_zip_pack.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
lib.isal_zip_pack.restype = ctypes.c_int
rc = lib.isal_zip_pack(b"out.docx", b"unpacked_docx", 1, 8)   # 0 表示成功
```

用完清理临时目录：

```python
lib.isal_zip_rmtree.argtypes = [ctypes.c_char_p]
rc = lib.isal_zip_rmtree(b"unpacked_docx")                    # 0 表示成功（目录已不在也算）
```

SAS 调用示例（SASCBTBL / `modulen`）：

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

## OOXML / OPC 要点

`.docx`/`.xlsx`/`.pptx` 本质是受 Open Packaging Conventions 约束的 ZIP。对写入器有两条关键规则：

1. 部件名是**包根相对路径**——顶层不允许再套一层文件夹，所以要用 `--root`。
2. **`[Content_Types].xml`** 必须是归档的**第一个条目**，`isal_zip` 已强制。

7-Zip/Word 还会做但**并非必需**的事（显式目录条目、把 content-types 流不压缩存储）——
现代 Office 能正常读取「所有部件都 Deflate、且 `[Content_Types].xml` 在首位」的 ZIP。
`isal_zip` 也写目录条目，并把压不小的部件改为存储。已用真实 Word 文档（含一张内嵌图片）
经 `isal_zip --root` 往返打包后在 Word 中重新打开验证。

未实现：加密、数据描述符（OOXML 都不需要）。

## 仓库结构

```
src/isal_zip.c        基于 Intel ISA-L isal_deflate_stateless() 的 ZIP 写入器
                      （同一份源码编成 isal_zip.exe 和 isal_zip.dll）
build.ps1 / build.sh  构建 isa-l.dll + isal_zip.exe + isal_zip.dll（PowerShell / MSYS2）
.github/workflows/    CI：在 windows-latest 上构建 + exe/dll smoke test
bench/bench_isal.py   ISA-L（python-isal）vs 7z vs zlib，单流
bench/bench_zip.py    isal_zip.exe vs 7z，文件或文件夹（端到端）
```

## 许可

BSD-3-Clause。Intel ISA-L 本身也是 BSD-3-Clause，且**未**内置在本仓库中，
由构建脚本按需拉取 / 见 <https://github.com/intel/isa-l>。
