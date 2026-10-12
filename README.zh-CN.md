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
* 目录递归打包；多线程（`ISAL_ZIP_THREADS`）。
* **ZIP64**——支持超过 4 GiB 的部件或归档、超过 65535 个条目。
* **UTF-8 文件名标志位**（通用位 11），自动为非 ASCII 名称置位。
* 写显式**目录条目**；当 Deflate 压不小某个部件时**回退为存储**（如已压缩的媒体）。
* **对 OOXML/OPC 安全**：`--root` 把目录内容放到归档根，并强制 `[Content_Types].xml` 为第一个条目。

每次 [Release](https://github.com/x15967311210-crypto/isal-zip/releases) 都附带预编译的
`isal_zip.exe` + `isa-l.dll`。

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
./build.sh                  # 克隆 ISA-L v2.32.1，编译 dll + exe
```

两个脚本都会克隆 Intel ISA-L（默认 `v2.32.1`）、编译 `isa-l.dll`，再编译
`src\isal_zip.c` 链接它。用 `-IsaLSrc` / `ISAL_SRC` 复用已有源码目录。
CI（`.github/workflows/ci.yml`）会在 `windows-latest` 上构建并跑 smoke test。

## 用法

```
isal_zip <level> <out.zip> [--root] <文件或目录1> [<文件或目录2> ...]

  level          ISA-L 压缩档位：0=store，1=最快，2，3=最优
  --root         把目录的“内容”放到归档根（OOXML/OPC 必需）
  环境变量 ISAL_ZIP_THREADS=N   并行压缩 N 个文件（默认 1）
  环境变量 ISAL_ZIP_FORCE64=1   始终输出 ZIP64 记录（用于测试）
```

示例：

```powershell
# 打包一个文件夹（保留顶层文件夹名）
.\isal_zip.exe 1 out.zip .\some_folder

# 打包解包后的 OOXML 包（内容放到根）-> 可正常打开的 .docx
.\isal_zip.exe 1 out.docx --root .\unpacked_docx
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
build.ps1 / build.sh  构建 isa-l.dll + isal_zip.exe（PowerShell / MSYS2）
.github/workflows/    CI：在 windows-latest 上构建 + smoke test
bench/bench_isal.py   ISA-L（python-isal）vs 7z vs zlib，单流
bench/bench_zip.py    isal_zip.exe vs 7z，文件或文件夹（端到端）
```

## 许可

BSD-3-Clause。Intel ISA-L 本身也是 BSD-3-Clause，且**未**内置在本仓库中，
由构建脚本按需拉取 / 见 <https://github.com/intel/isa-l>。
