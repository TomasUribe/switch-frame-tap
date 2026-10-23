# Build the Switch Frame Tap Windows viewer (local soak helper).
# Lives next to build-windows.sh under tools/raw-recv/.
#
# On first run (or when pieces are missing), downloads the ref\win toolchain:
#   zig 0.16, SDL2-devel mingw, libusb, FFmpeg LGPL shared (BtbN)
#
#   powershell -File tools/raw-recv/build-windows-viewer.ps1
#   tools\raw-recv\build-windows-viewer.bat
#
# Output (default):
#   E:\Desktop\SwitchU\- Switch Frame Tap -\build\Switch Frame Tap (Windows)
#
param(
    [string]$OutDir = "E:\Desktop\SwitchU\- Switch Frame Tap -\build\Switch Frame Tap (Windows)",
    [string]$Version = "",
    [switch]$SkipDeps,
    [switch]$ForceDeps
)

$ErrorActionPreference = "Stop"
# tools/raw-recv -> repo root (same layout as build-windows.sh)
$Repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $Version) {
    $Version = (git -C $Repo describe --tags --always --dirty 2>$null)
    if (-not $Version) { $Version = "local" }
    $Version = ($Version -replace '^v', '')
}

$W = Join-Path $Repo "ref\win"
$Cache = Join-Path $W "_cache"
$ZigDir = Join-Path $W "zig"
$SdlDir = Join-Path $W "sdl2"
$UsbDir = Join-Path $W "libusb"
$FfDir = Join-Path $W "ffmpeg"
$MinDir = Join-Path $W "ffmpeg-min"

# Pinned to match tools/raw-recv/build-windows.sh / ref/README.md
$ZigVer = "0.16.0"
$SdlVer = "2.32.10"
$UsbVer = "1.0.30"
$ZigUrl = "https://ziglang.org/download/$ZigVer/zig-x86_64-windows-$ZigVer.zip"
$SdlUrl = "https://github.com/libsdl-org/SDL/releases/download/release-$SdlVer/SDL2-devel-$SdlVer-mingw.tar.gz"
$UsbUrl = "https://github.com/libusb/libusb/releases/download/v$UsbVer/libusb-$UsbVer.7z"
$FfUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip"
$SevenZipUrl = "https://www.7-zip.org/a/7zr.exe"

function Write-Step([string]$msg) { Write-Host "[switch-frame-tap] $msg" }

function Ensure-Dir([string]$path) {
    if (-not (Test-Path $path)) { New-Item -ItemType Directory -Force -Path $path | Out-Null }
}

function Download-File([string]$Url, [string]$Dest) {
    Ensure-Dir (Split-Path -Parent $Dest)
    if ((Test-Path $Dest) -and -not $ForceDeps) {
        Write-Step "Cached: $(Split-Path -Leaf $Dest)"
        return
    }
    Write-Step "Downloading $(Split-Path -Leaf $Dest) ..."
    Write-Step "  $Url"
    $tmp = "$Dest.partial"
    if (Test-Path $tmp) { Remove-Item -Force $tmp }
    & curl.exe -fL --retry 3 --retry-delay 2 -o $tmp $Url
    if ($LASTEXITCODE -ne 0) { throw "Download failed: $Url" }
    Move-Item -Force $tmp $Dest
}

function Expand-ZipTo([string]$Zip, [string]$Dest) {
    if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
    Ensure-Dir $Dest
    Expand-Archive -LiteralPath $Zip -DestinationPath $Dest -Force
}

function Expand-TarGzTo([string]$Archive, [string]$Dest) {
    if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
    Ensure-Dir $Dest
    & tar.exe -xzf $Archive -C $Dest
    if ($LASTEXITCODE -ne 0) { throw "tar extract failed: $Archive" }
}

function Get-7zr {
    $seven = Join-Path $Cache "7zr.exe"
    if (-not (Test-Path $seven) -or $ForceDeps) {
        Download-File $SevenZipUrl $seven
    }
    return $seven
}

function Expand-7zTo([string]$Archive, [string]$Dest) {
    if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
    Ensure-Dir $Dest
    $seven = Get-7zr
    & $seven x -y "-o$Dest" $Archive | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z extract failed: $Archive" }
}

function Move-SingleChildUp([string]$Parent) {
    $kids = @(Get-ChildItem -LiteralPath $Parent -Force | Where-Object { $_.Name -ne '.' -and $_.Name -ne '..' })
    if ($kids.Count -eq 1 -and $kids[0].PSIsContainer) {
        $inner = $kids[0].FullName
        Get-ChildItem -LiteralPath $inner -Force | ForEach-Object {
            Move-Item -Force -LiteralPath $_.FullName -Destination (Join-Path $Parent $_.Name)
        }
        Remove-Item -Recurse -Force $inner
    }
}

function Test-ZigReady {
    return (Test-Path (Join-Path $ZigDir "zig.exe")) -or (Test-Path (Join-Path $ZigDir "zig"))
}
function Test-SdlReady {
    return (Test-Path (Join-Path $SdlDir "x86_64-w64-mingw32\bin\SDL2.dll"))
}
function Test-UsbReady {
    return (Test-Path (Join-Path $UsbDir "MinGW64\dll\libusb-1.0.dll")) -and
           (Test-Path (Join-Path $UsbDir "include"))
}
function Test-FfReady {
    if (Test-Path (Join-Path $MinDir "lib\libavcodec.a")) { return $true }
    return (Test-Path (Join-Path $FfDir "include")) -and (Test-Path (Join-Path $FfDir "bin"))
}

function Install-Zig {
    if ((Test-ZigReady) -and -not $ForceDeps) { return }
    Write-Step "Installing Zig $ZigVer into ref\win\zig ..."
    Ensure-Dir $Cache
    $zip = Join-Path $Cache "zig-x86_64-windows-$ZigVer.zip"
    Download-File $ZigUrl $zip
    $tmp = Join-Path $Cache "zig-extract"
    Expand-ZipTo $zip $tmp
    Move-SingleChildUp $tmp
    if (Test-Path $ZigDir) { Remove-Item -Recurse -Force $ZigDir }
    Move-Item -Force $tmp $ZigDir
    if (-not (Test-ZigReady)) { throw "Zig install failed (zig.exe missing)" }
}

function Install-Sdl {
    if ((Test-SdlReady) -and -not $ForceDeps) { return }
    Write-Step "Installing SDL2 $SdlVer (mingw) into ref\win\sdl2 ..."
    Ensure-Dir $Cache
    $tg = Join-Path $Cache "SDL2-devel-$SdlVer-mingw.tar.gz"
    Download-File $SdlUrl $tg
    $tmp = Join-Path $Cache "sdl2-extract"
    Expand-TarGzTo $tg $tmp
    Move-SingleChildUp $tmp
    if (Test-Path $SdlDir) { Remove-Item -Recurse -Force $SdlDir }
    Ensure-Dir $SdlDir
    $mingw = Join-Path $tmp "x86_64-w64-mingw32"
    if (-not (Test-Path $mingw)) { throw "SDL2 archive missing x86_64-w64-mingw32" }
    Move-Item -Force $mingw (Join-Path $SdlDir "x86_64-w64-mingw32")
    foreach ($lic in @("LICENSE.txt", "COPYING.txt", "README-SDL.txt")) {
        $p = Join-Path $tmp $lic
        if (Test-Path $p) { Copy-Item -Force $p (Join-Path $SdlDir $lic) }
    }
    Remove-Item -Recurse -Force $tmp
    if (-not (Test-SdlReady)) { throw "SDL2 install failed" }
}

function Install-Libusb {
    if ((Test-UsbReady) -and -not $ForceDeps) { return }
    Write-Step "Installing libusb $UsbVer into ref\win\libusb ..."
    Ensure-Dir $Cache
    $archive = Join-Path $Cache "libusb-$UsbVer.7z"
    Download-File $UsbUrl $archive
    $tmp = Join-Path $Cache "libusb-extract"
    Expand-7zTo $archive $tmp
    Move-SingleChildUp $tmp
    if (Test-Path $UsbDir) { Remove-Item -Recurse -Force $UsbDir }
    Move-Item -Force $tmp $UsbDir
    if (-not (Test-UsbReady)) { throw "libusb install failed (need include\ and MinGW64\)" }
}

function Install-Ffmpeg {
    if ((Test-FfReady) -and -not $ForceDeps) { return }
    Write-Step "Installing FFmpeg 8.1 LGPL shared into ref\win\ffmpeg ..."
    Ensure-Dir $Cache
    $zip = Join-Path $Cache "ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip"
    Download-File $FfUrl $zip
    $tmp = Join-Path $Cache "ffmpeg-extract"
    Expand-ZipTo $zip $tmp
    Move-SingleChildUp $tmp
    if (Test-Path $FfDir) { Remove-Item -Recurse -Force $FfDir }
    Move-Item -Force $tmp $FfDir
    if (-not (Test-FfReady)) { throw "FFmpeg install failed" }
}

function Ensure-Deps {
    if ($SkipDeps) {
        Write-Step "SkipDeps: not downloading toolchain"
        return
    }
    Ensure-Dir $W
    Ensure-Dir $Cache
    Write-Step "Checking Windows viewer deps under ref\win ..."
    Install-Zig
    Install-Sdl
    Install-Libusb
    Install-Ffmpeg
    Write-Step "Deps ready."
}

Ensure-Deps

$Zig = Join-Path $ZigDir "zig.exe"
if (-not (Test-Path $Zig)) { $Zig = Join-Path $ZigDir "zig" }
# Zig 0.16 splits its own lib path on spaces ("- Switch Frame Tap -"). Prefer a
# copy at C:\sft-zig when the repo lives under a spaced path (see build notes).
if ($Repo -match ' ') {
    $ZigNoSpace = "C:\sft-zig\zig.exe"
    if (Test-Path $ZigNoSpace) {
        Write-Step "Using C:\sft-zig (repo path has spaces; Zig 0.16 needs that)"
        $Zig = $ZigNoSpace
    }
}

foreach ($pair in @(
    @{ Name = "zig"; Ok = (Test-ZigReady) },
    @{ Name = "sdl2"; Ok = (Test-SdlReady) },
    @{ Name = "libusb"; Ok = (Test-UsbReady) }
)) {
    if (-not $pair.Ok) {
        Write-Step "Missing ref\win\$($pair.Name) after setup"
        Write-Step "Re-run without -SkipDeps, or use -ForceDeps to re-download."
        exit 1
    }
}
if (-not (Test-Path $Zig)) {
    Write-Step "zig not found under ref\win\zig"
    exit 1
}

$UseMin = Test-Path (Join-Path $MinDir "lib\libavcodec.a")
if (-not $UseMin -and -not (Test-FfReady)) {
    Write-Step "Need ref\win\ffmpeg-min or ref\win\ffmpeg"
    exit 1
}

$B = Join-Path $Repo "dist\build-windows-local"
if (Test-Path $B) { Remove-Item -Recurse -Force $B }
New-Item -ItemType Directory -Force -Path $B | Out-Null

$SdlInc = Join-Path $SdlDir "x86_64-w64-mingw32\include"
$SdlLib = Join-Path $SdlDir "x86_64-w64-mingw32\lib\libSDL2.dll.a"
$SdlDll = Join-Path $SdlDir "x86_64-w64-mingw32\bin\SDL2.dll"
$UsbInc = Join-Path $UsbDir "include"
$UsbLib = Join-Path $UsbDir "MinGW64\static\libusb-1.0.dll.a"
$UsbDll = Join-Path $UsbDir "MinGW64\dll\libusb-1.0.dll"
$Src = Join-Path $Repo "tools\raw-recv\raw-view.c"
$Exe = Join-Path $B "SwitchFrameTap.exe"

Write-Step "Compiling Windows viewer ($Version)..."
$cc = @(
    "cc", "-target", "x86_64-windows-gnu", "-O2", "-Wall", "-Wextra",
    "-DSFT_H264", "-DSFT_MP4",
    "-I", $SdlInc, "-I", $UsbInc
)
if ($UseMin) {
    $cc += @("-I", (Join-Path $MinDir "include"))
    $ffLibs = @(
        (Join-Path $MinDir "lib\libavformat.a"),
        (Join-Path $MinDir "lib\libavcodec.a"),
        (Join-Path $MinDir "lib\libavutil.a"),
        "-lbcrypt"
    )
    Write-Step "FFmpeg: static (ref\win\ffmpeg-min)"
} else {
    $cc += @("-I", (Join-Path $FfDir "include"))
    $ffLibs = @(
        (Join-Path $FfDir "lib\libavcodec.dll.a"),
        (Join-Path $FfDir "lib\libavutil.dll.a"),
        (Join-Path $FfDir "lib\libavformat.dll.a")
    )
    Write-Step "FFmpeg: shared DLLs (ref\win\ffmpeg)"
}

& $Zig @cc $Src $SdlLib $UsbLib @ffLibs "-lshell32" "-lws2_32" "-Wl,--subsystem,windows" "-o" $Exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item -Force $Exe $OutDir
# Soak / desktop shortcut name - same binary as SwitchFrameTap.exe.
# If -new is locked (viewer still running), leave a fresh sidecar instead of
# silently keeping an old Oct-era binary that the shortcut used to prefer.
$NewExe = Join-Path $OutDir "SwitchFrameTap-new.exe"
try {
    Copy-Item -Force $Exe $NewExe
} catch {
    $sidecar = Join-Path $OutDir "SwitchFrameTap-fresh.exe"
    Copy-Item -Force $Exe $sidecar
    Write-Step "SwitchFrameTap-new.exe is locked (viewer open?) - wrote SwitchFrameTap-fresh.exe"
    Write-Step "Close the viewer, then re-run this build (or launch SwitchFrameTap.exe / -fresh)."
}
foreach ($dll in @($SdlDll, $UsbDll)) {
    try { Copy-Item -Force $dll $OutDir }
    catch { Write-Step "Skipped $(Split-Path -Leaf $dll) (locked - viewer open?)" }
}
if (-not $UseMin) {
    foreach ($pat in @("avcodec-*.dll", "avutil-*.dll", "avformat-*.dll", "swresample-*.dll")) {
        $hit = Get-ChildItem (Join-Path $FfDir "bin") -Filter $pat -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) {
            try { Copy-Item -Force $hit.FullName $OutDir }
            catch { Write-Step "Skipped $($hit.Name) (locked - viewer open?)" }
        }
    }
}
$ReadmeSrc = Join-Path $Repo "tools\raw-recv\README-WINDOWS.txt"
if (Test-Path $ReadmeSrc) {
    Copy-Item -Force $ReadmeSrc (Join-Path $OutDir "README-WINDOWS.txt")
} elseif (-not (Test-Path (Join-Path $OutDir "README-WINDOWS.txt"))) {
    @"
Switch Frame Tap - Windows viewer
Double-click SwitchFrameTap.exe. Keep the DLLs next to it.
Settings: Esc. Borderless: B (or Settings). Drag the picture to move when borderless.
"@ | Set-Content -Encoding UTF8 (Join-Path $OutDir "README-WINDOWS.txt")
}

Write-Host ""
Write-Step "Windows viewer ready:"
Write-Step "  $OutDir"
Write-Step "Run SwitchFrameTap-new.exe (or SwitchFrameTap.exe) from that folder (DLLs must stay beside it)."
