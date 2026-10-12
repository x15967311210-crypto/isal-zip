<#
.SYNOPSIS
    Build Intel ISA-L (isa-l.dll) and isal_zip.exe on Windows.

.DESCRIPTION
    isal_zip links Intel ISA-L's igzip Deflate engine, so ISA-L must be compiled
    first. This script:
      1. clones Intel ISA-L (unless -IsaLSrc points at an existing checkout)
      2. configures + builds isa-l.dll with CMake + NASM + MinGW gcc
      3. compiles src\isal_zip.c and links it against isa-l.dll

    Requirements on PATH (or pass explicit paths): nasm, gcc (MinGW-w64/x86_64),
    cmake, the generator's build tool (mingw32-make for "MinGW Makefiles", ninja
    for "Ninja"), git.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Generator Ninja -Gcc C:\msys64\ucrt64\bin\gcc.exe
#>
param(
    [string]$Work      = (Join-Path $PSScriptRoot ".build"),
    [string]$IsaLSrc   = "",
    [string]$Nasm      = "nasm",
    [string]$Gcc       = "gcc",
    [string]$CMake     = "cmake",
    [string]$Generator = "MinGW Makefiles",
    [string]$ISALRef   = "v2.32.1"
)
$ErrorActionPreference = "Continue"   # native tools write to stderr; we check $LASTEXITCODE

function Resolve-Tool($cmd) {
    $c = Get-Command $cmd -ErrorAction SilentlyContinue
    if (-not $c) { throw "not found on PATH: $cmd" }
    return $c.Source
}
$Nasm  = Resolve-Tool $Nasm
$Gcc   = Resolve-Tool $Gcc
$CMake = Resolve-Tool $CMake

New-Item -ItemType Directory -Force $Work | Out-Null

if (-not $IsaLSrc) {
    $IsaLSrc = Join-Path $Work "isa-l"
    if (-not (Test-Path (Join-Path $IsaLSrc "igzip"))) {
        Write-Host "Cloning Intel ISA-L $ISALRef ..."
        git clone --depth 1 --branch $ISALRef https://github.com/intel/isa-l.git $IsaLSrc
        if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
    }
}

$bld = Join-Path $Work "build"
Remove-Item -Recurse -Force $bld -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $bld | Out-Null

Write-Host "Configuring ISA-L (generator: $Generator) ..."
& $CMake -G $Generator -S $IsaLSrc -B $bld `
    "-DCMAKE_C_COMPILER=$Gcc" `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_ASM_NASM_COMPILER=$Nasm" `
    "-DISAL_BUILD_TESTS=OFF" `
    "-DISAL_BUILD_PERF_TESTS=OFF" `
    "-DISAL_BUILD_ISAL_SHIM=OFF"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "Building isa-l.dll ..."
& $CMake --build $bld -j
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$dll = Join-Path $bld "isa-l.dll"
$imp = Join-Path $bld "isa-l.dll.a"
if (-not (Test-Path $dll)) { throw "isa-l.dll not produced" }

Write-Host "Building isal_zip.exe ..."
$out = Join-Path $PSScriptRoot "isal_zip.exe"
& $Gcc -O2 (Join-Path $PSScriptRoot "src\isal_zip.c") $imp `
    -I (Join-Path $bld "build_include") `
    -I (Join-Path $IsaLSrc "include") `
    -o $out
if ($LASTEXITCODE -ne 0) { throw "isal_zip compile failed" }

Copy-Item $dll $PSScriptRoot -Force   # ship isa-l.dll next to the exe
Write-Host ""
Write-Host "OK -> $out" -ForegroundColor Green
Write-Host "     $PSScriptRoot\isa-l.dll"
Write-Host "Try:  .\isal_zip.exe 1 out.zip --root <folder>"
