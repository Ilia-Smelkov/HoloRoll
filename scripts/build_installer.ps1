<#
.SYNOPSIS
    Builds HoloRoll Release DLL and packages it into an Inno Setup installer.

.DESCRIPTION
    Run from the repository root:
        .\scripts\build_installer.ps1

    Steps performed:
      1. Configure & build CMake preset 'x64-Release'.
      2. Stage reaper_holoroll.dll to installer\payload\.
      3. Invoke ISCC.exe with /DMyAppVersion=<version> to compile the installer.
      4. Output:  dist\HoloRoll-Setup-<version>.exe

.PARAMETER Version
    Semantic version string injected into the installer (e.g. "0.1.0").
    Defaults to the value in CMakeLists.txt's project() declaration.

.PARAMETER IsccPath
    Path to ISCC.exe. Defaults to a typical Inno Setup 6 install location.

.PARAMETER ReaperSdkDir
    Override REAPER SDK location. Defaults to env:REAPER_SDK_DIR or the
    vendored copy under third_party\reaper-sdk\sdk.

.PARAMETER SkipBuild
    Use an existing build (skip CMake configure + build). Useful when
    iterating on the installer script alone.
#>

param(
  [Parameter(Mandatory = $false)]
  [string]$Version,

  [Parameter(Mandatory = $false)]
  [string]$IsccPath,

  [Parameter(Mandatory = $false)]
  [string]$ReaperSdkDir = $env:REAPER_SDK_DIR,

  [Parameter(Mandatory = $false)]
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $repoRoot
try {
  # ---- Resolve version from CMakeLists if not supplied ----------------------
  if (-not $Version) {
    $cmake = Get-Content (Join-Path $repoRoot "CMakeLists.txt") -Raw
    if ($cmake -match 'project\(\s*\S+\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
      $Version = $Matches[1]
      Write-Host "Detected version from CMakeLists.txt: $Version"
    } else {
      throw "Could not auto-detect version from CMakeLists.txt. Pass -Version explicitly."
    }
  }

  # ---- Build the DLL --------------------------------------------------------
  if (-not $SkipBuild) {
    if (-not $ReaperSdkDir) {
      $localSdk = Join-Path $repoRoot "third_party\reaper-sdk\sdk"
      if (Test-Path (Join-Path $localSdk "reaper_plugin.h")) {
        $ReaperSdkDir = (Resolve-Path $localSdk).Path
      } else {
        throw "REAPER SDK not found. Set REAPER_SDK_DIR or vendor the SDK."
      }
    }
    $env:REAPER_SDK_DIR = $ReaperSdkDir

    Write-Host "Configuring CMake (x64-Release)..."
    cmake --preset x64-Release
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

    Write-Host "Building Release..."
    cmake --build --preset build-release
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
  } else {
    Write-Host "-SkipBuild was set; assuming Release DLL already exists."
  }

  $dllPath = Join-Path $repoRoot "build\x64-Release\Release\reaper_holoroll.dll"
  if (-not (Test-Path $dllPath)) {
    throw "DLL not found at $dllPath after build. Aborting."
  }

  # ---- Stage payload --------------------------------------------------------
  $payloadDir = Join-Path $repoRoot "installer\payload"
  if (-not (Test-Path $payloadDir)) {
    New-Item -ItemType Directory -Path $payloadDir | Out-Null
  }
  Copy-Item -Path $dllPath -Destination (Join-Path $payloadDir "reaper_holoroll.dll") -Force
  Write-Host "Staged DLL -> installer\payload\reaper_holoroll.dll"

  # ---- Locate ISCC ----------------------------------------------------------
  if (-not $IsccPath) {
    $candidates = @(
      "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
      "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    ) | Where-Object { $_ }
    foreach ($c in $candidates) {
      if (Test-Path $c) { $IsccPath = $c; break }
    }
  }
  if (-not $IsccPath -or -not (Test-Path $IsccPath)) {
    throw "ISCC.exe not found. Install Inno Setup 6 from https://jrsoftware.org/isdl.php or pass -IsccPath."
  }

  # ---- Output dir -----------------------------------------------------------
  $distDir = Join-Path $repoRoot "dist"
  if (-not (Test-Path $distDir)) {
    New-Item -ItemType Directory -Path $distDir | Out-Null
  }

  # ---- Compile installer ----------------------------------------------------
  $issPath = Join-Path $repoRoot "installer\holoroll.iss"
  Write-Host "Compiling installer ($Version)..."
  & $IsccPath "/DMyAppVersion=$Version" "/Qp" $issPath
  if ($LASTEXITCODE -ne 0) { throw "ISCC failed (exit $LASTEXITCODE)." }

  $installer = Join-Path $distDir "HoloRoll-Setup-$Version.exe"
  if (-not (Test-Path $installer)) {
    throw "Compilation succeeded but installer not found at expected path: $installer"
  }
  $size = [math]::Round((Get-Item $installer).Length / 1KB, 1)
  Write-Host ""
  Write-Host "Installer ready: $installer ($size KB)"
}
finally {
  Pop-Location
}
