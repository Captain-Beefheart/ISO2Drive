# Build the standalone ISO2Drive GUI exe (Nuitka onefile; bundles Python + tkinter
# + the CLI engine, so it's a single double-click file with no Python required).
#
# IMPORTANT: use a portable python-build-standalone CPython 3.12 with `nuitka`
# installed -- the MSYS2 mingw Python 3.14 does NOT work with Nuitka (os.sep=='/').
#   1) download an astral-sh python-build-standalone CPython 3.12 (…install_only.tar.gz)
#   2) <that python> -m pip install nuitka ordered-set zstandard
#
# Usage:
#   packaging/build-gui-exe.ps1 -Python C:\path\to\pbs\python\python.exe `
#       [-Engine .\iso2drive-0.2.0-windows-x64.exe] [-Gcc C:\msys64\mingw64\bin\gcc.exe]
param(
    [Parameter(Mandatory = $true)] [string]$Python,
    [string]$Engine = "",
    [string]$Gcc = "C:\msys64\mingw64\bin\gcc.exe",
    [string]$OutDir = "$PSScriptRoot\..\dist"
)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."

if (-not $Engine) {
    $Engine = (Get-ChildItem "$root\iso2drive*.exe" |
        Where-Object { $_.Name -notlike '*gui*' } | Select-Object -First 1).FullName
}
if (-not $Engine) { throw "No CLI engine found. Build it first (make) or pass -Engine." }

$build = Join-Path $OutDir "guibuild"
New-Item -ItemType Directory -Force $OutDir, $build | Out-Null
Copy-Item $Engine "$build\iso2drive.exe" -Force  # bundled name the GUI looks for

$env:CC = $Gcc.Replace('\', '/')
$env:Path = (Split-Path $Gcc) + ";$env:Path"

& $Python -m nuitka --onefile --enable-plugin=tk-inter --windows-console-mode=disable `
    --experimental=force-accept-windows-gcc --assume-yes-for-downloads `
    --include-data-files="$build\iso2drive.exe=iso2drive.exe" `
    --output-dir="$build" --output-filename="iso2drive-gui.exe" --remove-output `
    "$root\gui\iso2drive_gui.py"

Copy-Item "$build\iso2drive-gui.exe" "$OutDir\iso2drive-gui.exe" -Force
Write-Host "Built: $OutDir\iso2drive-gui.exe"
