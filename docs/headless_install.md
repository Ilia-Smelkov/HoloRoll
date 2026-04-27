# Headless installation

The HoloRoll installer is built with [Inno Setup](https://jrsoftware.org/),
which exposes a stable command-line interface for unattended invocation. This
makes it safe to embed inside other tooling (game-engine bridges, art-pipeline
launchers, build farms).

## Usage

```text
HoloRoll-Setup-<version>.exe [options]
```

Common options:

| Flag | Effect |
|------|--------|
| `/SILENT` | Show only a tiny progress window. No prompts. |
| `/VERYSILENT` | Show nothing at all. No window, no prompts. |
| `/SUPPRESSMSGBOXES` | Silently dismiss any informational message boxes that would otherwise block. |
| `/DIR="<path>"` | Override the install directory. Useful for portable REAPER setups. |
| `/LOG="<file>"` | Write a verbose installation log. |
| `/NORESTART` | Suppress any restart prompts (HoloRoll currently never asks for one). |
| `/NOICONS` | Skip optional Start menu entries (HoloRoll has none anyway). |

A typical headless invocation from another launcher:

```powershell
$exitCode = (Start-Process `
  -FilePath "HoloRoll-Setup-0.1.0.exe" `
  -ArgumentList @("/SILENT", "/SUPPRESSMSGBOXES", "/LOG=`"$env:TEMP\holoroll-install.log`"") `
  -Wait -PassThru).ExitCode

if ($exitCode -ne 0) {
  Write-Error "HoloRoll install failed (code $exitCode). See $env:TEMP\holoroll-install.log"
}
```

## Exit codes

The installer follows Inno Setup's standard exit codes:

| Code | Meaning |
|------|---------|
| `0`  | Success. |
| `1`  | Setup was cancelled (user clicked Cancel, or the user-elevation prompt was declined). |
| `2`  | The setup failed during the uninstaller phase. |
| `3`  | Prepare-to-install failed. **The most common reason: REAPER was running and could not be closed.** Other causes include unwritable target folder. |
| `4`  | Prepare-to-install failed and the previous install must be uninstalled first. |
| `5`  | The user clicked Cancel during prepare-to-install. |

Any non-zero exit code means setup did not complete. For headless launchers,
the practical behaviour is:

- `0` → success, plugin is in place.
- `3` → most likely REAPER is open. Close it and retry.
- anything else → use `/LOG` to capture details.

Reference: <https://jrsoftware.org/ishelp/topic_setupexitcodes.htm>.

## What the installer does

1. **Locate the REAPER plugin folder.** Order:
   - Reads `HKCU\Software\Cockos\REAPER` for portable REAPER paths.
   - Falls back to `%APPDATA%\REAPER\UserPlugins`.
2. **Stop running REAPER.** If REAPER is open and the install isn't silent,
   asks for permission first. In silent mode it forces shutdown via
   `taskkill /F /IM reaper.exe`. If that fails, setup aborts with exit code 3.
3. **Remove the legacy MVP plugin** (`reaper_mdd_viewport.dll`) if present —
   prevents two copies of HoloRoll loading at once.
4. **Copy** `reaper_holoroll.dll` into the resolved plugin folder, plus a
   text copy of the README and LICENSE.
5. **Register an uninstaller** in *Apps & features* under "HoloRoll <version>".

## Building the installer locally

You need:

- Visual Studio 2022 with the Desktop C++ workload.
- CMake 3.24+.
- [Inno Setup 6](https://jrsoftware.org/isdl.php) (for `ISCC.exe`).

Then:

```powershell
.\scripts\build_installer.ps1
# -> dist\HoloRoll-Setup-<version>.exe
```

The script auto-detects the version from `CMakeLists.txt`. To override:

```powershell
.\scripts\build_installer.ps1 -Version 0.1.0
```

## Releases pipeline (CI)

`v*.*.*` tags pushed to GitHub trigger
[`.github/workflows/release.yml`](../.github/workflows/release.yml), which:

1. Builds Release.
2. Compiles the installer with the tag's version.
3. Creates a GitHub Release and attaches the installer as a downloadable
   asset.

To cut a release locally:

```powershell
git tag v0.1.0
git push origin v0.1.0
```

The Release page appears at
<https://github.com/Ilia-Smelkov/HoloRoll/releases/tag/v0.1.0> within a few
minutes.
