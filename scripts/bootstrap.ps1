param(
  [Parameter(Mandatory = $false)]
  [string]$ReaperSdkDir = $env:REAPER_SDK_DIR,
  [Parameter(Mandatory = $false)]
  [ValidateSet("x64-Debug", "x64-Release")]
  [string]$Preset = "x64-Debug",
  [Parameter(Mandatory = $false)]
  [switch]$DeployToReaper,
  [Parameter(Mandatory = $false)]
  [string]$ReaperUserPluginsDir = (Join-Path $env:APPDATA "REAPER\UserPlugins"),
  [Parameter(Mandatory = $false)]
  [string]$ReaperEffectsDir = (Join-Path $env:APPDATA "REAPER\Effects"),
  [Parameter(Mandatory = $false)]
  [switch]$KillReaper,
  [Parameter(Mandatory = $false)]
  [switch]$RestartReaper,
  [Parameter(Mandatory = $false)]
  [string]$ReaperExePath
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ReaperSdkDir)) {
  $localSdk = Join-Path $PSScriptRoot "..\third_party\reaper-sdk\sdk"
  $resolvedLocalSdk = (Resolve-Path $localSdk -ErrorAction SilentlyContinue)
  if ($resolvedLocalSdk) {
    $ReaperSdkDir = $resolvedLocalSdk.Path
  } else {
    throw "REAPER SDK not found. Provide -ReaperSdkDir or clone to third_party/reaper-sdk/sdk."
  }
}

$headerPath = Join-Path $ReaperSdkDir "reaper_plugin.h"
if (-not (Test-Path $headerPath)) {
  throw "Cannot find reaper_plugin.h in '$ReaperSdkDir'."
}

$env:REAPER_SDK_DIR = $ReaperSdkDir

Write-Host "Configuring CMake preset: $Preset"
cmake --preset $Preset

Write-Host "Building preset: $Preset"
if ($Preset -eq "x64-Release") {
  cmake --build --preset build-release
} else {
  cmake --build --preset build-debug
}

$configuration = if ($Preset -eq "x64-Release") { "Release" } else { "Debug" }
$dllPath = Join-Path $PSScriptRoot "..\build\$Preset\$configuration\reaper_holoroll.dll"
$resolvedDll = Resolve-Path $dllPath -ErrorAction SilentlyContinue

if (-not $resolvedDll) {
  throw "Build finished, but DLL was not found at '$dllPath'."
}

Write-Host "Done. DLL: $($resolvedDll.Path)"

if ($KillReaper -or $RestartReaper) {
  Get-Process reaper -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 300
  Write-Host "REAPER processes were stopped (if running)."
}

if ($DeployToReaper) {
  if ([string]::IsNullOrWhiteSpace($ReaperUserPluginsDir)) {
    throw "ReaperUserPluginsDir is empty."
  }

  New-Item -ItemType Directory -Force -Path $ReaperUserPluginsDir | Out-Null
  $destinationDll = Join-Path $ReaperUserPluginsDir "reaper_holoroll.dll"
  Copy-Item -Path $resolvedDll.Path -Destination $destinationDll -Force
  Write-Host "Deployed to: $destinationDll"

  # Best-effort cleanup of the legacy MVP filename.
  $legacyDll = Join-Path $ReaperUserPluginsDir "reaper_mdd_viewport.dll"
  if (Test-Path $legacyDll) {
    Remove-Item $legacyDll -Force -ErrorAction SilentlyContinue
    Write-Host "Removed legacy plugin: $legacyDll"
  }

  # v0.12.0-alpha.3: also deploy bundled JSFX assets. The HoloRoll
  # extension inserts these JSFX onto a dedicated track to host per-bone
  # motion envelopes (REAPER envelopes always need a parameter target,
  # and JSFX sliders are the cheapest way to provide one). Without this
  # copy step, REAPER won't see the plugin even after the extension is
  # loaded.
  $jsfxSourceDir = Join-Path $PSScriptRoot "..\assets\effects\HoloRoll"
  $jsfxSourceResolved = Resolve-Path $jsfxSourceDir -ErrorAction SilentlyContinue
  if ($jsfxSourceResolved) {
    $jsfxDestDir = Join-Path $ReaperEffectsDir "HoloRoll"
    New-Item -ItemType Directory -Force -Path $jsfxDestDir | Out-Null
    Copy-Item -Path (Join-Path $jsfxSourceResolved.Path "*") -Destination $jsfxDestDir -Recurse -Force
    Write-Host "Deployed JSFX assets to: $jsfxDestDir"
  } else {
    Write-Warning "JSFX assets directory not found at '$jsfxSourceDir' -- skipping."
  }
}

if ($RestartReaper) {
  if ([string]::IsNullOrWhiteSpace($ReaperExePath)) {
    $candidates = @(
      "C:\Program Files\REAPER (x64)\reaper.exe",
      "C:\Program Files\REAPER\reaper.exe",
      "C:\Program Files (x86)\REAPER\reaper.exe",
      (Join-Path $env:LOCALAPPDATA "Programs\REAPER\reaper.exe")
    )
    foreach ($candidate in $candidates) {
      if (Test-Path $candidate) {
        $ReaperExePath = $candidate
        break
      }
    }
  }

  if ([string]::IsNullOrWhiteSpace($ReaperExePath) -or -not (Test-Path $ReaperExePath)) {
    throw "Cannot find reaper.exe automatically. Pass -ReaperExePath."
  }

  Start-Process -FilePath $ReaperExePath
  Write-Host "REAPER started: $ReaperExePath"
}
