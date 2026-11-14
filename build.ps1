# 흑갈리(Windows)에서 빌드: 폴더 안 MinGW-w64 GCC + Git for Windows 의 sh(Makefile 의 mkdir·rm 용).
# 시스템 PATH 는 바꾸지 않고 이 스크립트 프로세스 안에서만 쓴다.
#   .\build.ps1          앱 빌드 → build\puppypi_heukgalli.exe
#   .\build.ps1 test     코어 단위 시험
#   .\build.ps1 run      빌드 후 실행
param([string]$Target = "app")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$gcc = Join-Path $root "toolchain\mingw64\bin"
$sh = "C:\Program Files\Git\usr\bin"
if (-not (Test-Path (Join-Path $gcc "g++.exe"))) { throw "컴파일러가 없습니다: $gcc" }
if (-not (Test-Path (Join-Path $sh "sh.exe"))) { throw "Git for Windows 의 sh.exe 가 필요합니다: $sh" }
$env:PATH = "$gcc;$sh;$env:PATH"
Push-Location $root
try {
    $make = if ($Target -eq "run") { "app" } else { $Target }
    & mingw32-make -j8 $make
    if ($LASTEXITCODE -ne 0) { throw "빌드 실패 ($LASTEXITCODE)" }
    if ($Target -eq "run") { Start-Process (Join-Path $root "build\puppypi_heukgalli.exe") }
} finally {
    Pop-Location
}
