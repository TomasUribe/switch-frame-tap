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
if (Test-Path $B) {
    try { Remove-Item -Recurse -Force $B }
    catch {
        $B = Join-Path $Repo ("dist\build-windows-local-{0}" -f (Get-Date -Format "HHmmss"))
        Write-Step "Previous build dir locked - using $B"
    }
}
New-Item -ItemType Directory -Force -Path $B | Out-Null

$SdlInc = Join-Path $SdlDir "x86_64-w64-mingw32\include"
$SdlLib = Join-Path $SdlDir "x86_64-w64-mingw32\lib\libSDL2.dll.a"
$SdlDll = Join-Path $SdlDir "x86_64-w64-mingw32\bin\SDL2.dll"
$UsbInc = Join-Path $UsbDir "include"
$UsbLib = Join-Path $UsbDir "MinGW64\static\libusb-1.0.dll.a"
$UsbDll = Join-Path $UsbDir "MinGW64\dll\libusb-1.0.dll"
$Src = Join-Path $Repo "tools\raw-recv\raw-view.c"
$Svg = Join-Path $Repo "tools\raw-recv\switch-frame-tap.svg"
$IcoSrc = Join-Path $Repo "tools\raw-recv\switch-frame-tap.ico"
$Ico = Join-Path $B "switch-frame-tap.ico"
$Rc = Join-Path $B "raw-view.rc"
$Exe = Join-Path $B "SwitchFrameTap.exe"

# Same as build-windows.sh: embed the Frame Tap icon + VERSIONINFO in the .exe
# (Explorer / taskbar / Alt-Tab). Prefer committed .ico; else ImageMagick from SVG.
if (Test-Path $IcoSrc) {
    Copy-Item -Force $IcoSrc $Ico
    Write-Step "Icon: tools\raw-recv\switch-frame-tap.ico"
} else {
    $Magick = $null
    foreach ($c in @(
        "${env:ProgramFiles}\ImageMagick-*\magick.exe",
        "${env:ProgramFiles(x86)}\ImageMagick-*\magick.exe"
    )) {
        $hit = Get-Item $c -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { $Magick = $hit.FullName; break }
    }
    if (-not $Magick) {
        $cmd = Get-Command magick.exe -ErrorAction SilentlyContinue
        if ($cmd) { $Magick = $cmd.Source }
    }
    if (-not $Magick -or -not (Test-Path $Svg)) {
        throw "Need tools\raw-recv\switch-frame-tap.ico (or ImageMagick magick.exe + switch-frame-tap.svg)"
    }
    Write-Step "Icon: generating .ico from SVG via ImageMagick"
    & $Magick -background none -density 384 $Svg -define "icon:auto-resize=256,64,48,32,16" $Ico
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Ico)) { throw "ImageMagick failed to write $Ico" }
}

# VERSIONINFO numbers: 0.7.19-1-g0b0a202-dirty -> 0,7,19,0
$verNums = @("0", "0", "0", "0")
$m = [regex]::Match($Version, '^(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:\.(\d+))?')
if ($m.Success) {
    for ($i = 1; $i -le 4; $i++) {
        if ($m.Groups[$i].Success -and $m.Groups[$i].Value) { $verNums[$i - 1] = $m.Groups[$i].Value }
    }
}
$VCsv = ($verNums -join ",")
$RcText = @"
1 ICON "switch-frame-tap.ico"
1 VERSIONINFO
FILEVERSION $VCsv
PRODUCTVERSION $VCsv
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "switch-frame-tap"
      VALUE "FileDescription", "Switch Frame Tap - viewer"
      VALUE "FileVersion", "$Version"
      VALUE "ProductName", "Switch Frame Tap"
      VALUE "ProductVersion", "$Version"
      VALUE "LegalCopyright", "GPL-2.0 - github.com/TomasUribe/switch-frame-tap"
      VALUE "OriginalFilename", "SwitchFrameTap.exe"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@
# Zig's Windows RC path expects forward slashes / a cwd-relative ico name.
Set-Content -Encoding ascii -Path $Rc -Value $RcText

Write-Step "Compiling Windows viewer ($Version)..."
# Pass VERSION through a tiny header so PowerShell cannot strip the C string quotes
# (zig argv -DSFT_VERSION="x" loses quotes under Windows PowerShell).
$VerH = Join-Path $B "sft_version.h"
@(
    "#pragma once"
    ('#define SFT_VERSION "{0}"' -f $Version)
) | Set-Content -Encoding ascii -Path $VerH
$cc = @(
    "cc", "-target", "x86_64-windows-gnu", "-O2", "-Wall", "-Wextra",
    "-DSFT_H264", "-DSFT_MP4",
    "-include", $VerH,
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

Push-Location $B
try {
    & $Zig @cc $Src $Rc $SdlLib $UsbLib @ffLibs "-lshell32" "-lws2_32" "-Wl,--subsystem,windows" "-o" $Exe
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$DestExe = Join-Path $OutDir "SwitchFrameTap.exe"
# Only SwitchFrameTap.exe - if the viewer is open, stop it so we can overwrite.
function Stop-ViewerLocks {
    Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -like "SwitchFrameTap*" } |
        ForEach-Object {
            Write-Step "Stopping $($_.ProcessName) (pid $($_.Id)) so SwitchFrameTap.exe can be overwritten"
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        }
    Start-Sleep -Milliseconds 400
}
try {
    Copy-Item -Force $Exe $DestExe
} catch {
    Stop-ViewerLocks
    Copy-Item -Force $Exe $DestExe
}
foreach ($dll in @($SdlDll, $UsbDll)) {
    try { Copy-Item -Force $dll $OutDir }
    catch {
        Stop-ViewerLocks
        Copy-Item -Force $dll $OutDir
    }
}
if (-not $UseMin) {
    foreach ($pat in @("avcodec-*.dll", "avutil-*.dll", "avformat-*.dll", "swresample-*.dll")) {
        $hit = Get-ChildItem (Join-Path $FfDir "bin") -Filter $pat -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) {
            try { Copy-Item -Force $hit.FullName $OutDir }
            catch {
                Stop-ViewerLocks
                Copy-Item -Force $hit.FullName $OutDir
            }
        }
    }
}
# Drop old soak sidecars if still sitting next to the real exe.
foreach ($stale in @("SwitchFrameTap-new.exe", "SwitchFrameTap-fresh.exe", "SwitchFrameTap-reconnect.exe")) {
    $p = Join-Path $OutDir $stale
    if (Test-Path $p) {
        try { Remove-Item -Force $p; Write-Step "Removed stale $stale" }
        catch { Write-Step "Could not remove $stale (locked?)" }
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
Write-Step "Run SwitchFrameTap.exe from that folder (DLLs must stay beside it)."
