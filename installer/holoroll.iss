; HoloRoll installer — built with Inno Setup 6+.
;
; Build via scripts\build_installer.ps1, which fills the DLL into
; installer\payload\reaper_holoroll.dll before invoking ISCC.
;
; Headless usage:
;   HoloRoll-Setup-x.y.z.exe /SILENT
;   HoloRoll-Setup-x.y.z.exe /VERYSILENT /SUPPRESSMSGBOXES /LOG="install.log"
;   HoloRoll-Setup-x.y.z.exe /SILENT /DIR="D:\REAPER\UserPlugins"
;
; Headless behaviour (alpha.12+): the installer does NOT touch REAPER in
; silent mode. The caller (a first-time bootstrapper, or an in-app
; auto-updater) is expected to make sure REAPER is closed or that locked
; files won't be a problem. If a file is in use, Inno's standard
; "file in use" handling fires and the install may fail with a non-zero
; exit code. UI mode (interactive double-click) still prompts the user
; about REAPER and offers to close it, unchanged.
;
; Exit codes (standard Inno Setup):
;   0   success
;   1   setup was cancelled, or InitializeSetup returned False
;   2   uninstaller crashed
;   3   prepare-to-install failed (only fires in UI mode now —
;       happens when user refuses to close REAPER at the prompt;
;       silent mode never returns 3 anymore)
;   4   prepare-to-install failed and uninstaller is required
;   5   user clicked Cancel during prepare-to-install
;
; Any non-zero code means setup did not complete. Use /LOG to capture
; details for debugging.
;
; See https://jrsoftware.org/ishelp/topic_setupexitcodes.htm

#define MyAppName        "HoloRoll"
#define MyAppPublisher   "Muted Games"
#define MyAppURL         "https://github.com/Ilia-Smelkov/HoloRoll"
#define MyDllName        "reaper_holoroll.dll"
#define MyLegacyDllName  "reaper_mdd_viewport.dll"

; Version is injected from the command line by build_installer.ps1
; (local) or the GitHub Actions release workflow:
;   ISCC /DMyAppVersion=0.13.0-alpha.4 /DMyFileVersion=0.13.0 holoroll.iss
;
; MyAppVersion (full, may include "-alpha.N"): user-facing label —
;   AppVersion, UninstallDisplayName, installer filename.
; MyFileVersion (strict X.Y.Z, alpha.4+): goes into VersionInfoVersion,
;   which becomes the Windows file-version metadata (Properties dialog).
;   Inno rejects pre-release suffixes there. Defaults to MyAppVersion if
;   not supplied, so local build_installer.ps1 doesn't have to change
;   — but tagged alpha releases MUST set MyFileVersion explicitly.
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0-dev"
#endif
#ifndef MyFileVersion
  #define MyFileVersion MyAppVersion
#endif

; Output directory & filename, also overridable from the command line.
#ifndef MyOutputDir
  #define MyOutputDir "..\dist"
#endif
#ifndef MyOutputBaseFilename
  #define MyOutputBaseFilename "HoloRoll-Setup-" + MyAppVersion
#endif

[Setup]
; A stable AppId so future installers replace previous ones cleanly.
; Changing this string would create a parallel installation — do not edit.
AppId={{A2D4D9F6-3E2C-4E51-8F8C-31B6F3B0D8B7}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
VersionInfoVersion={#MyFileVersion}.0
; The plugin folder is determined automatically (REAPER's UserPlugins).
; Users almost never need to override it; for portable REAPER setups they
; can pass /DIR=... on the command line.
DefaultDirName={code:DefaultPluginsDir}
DisableDirPage=yes
; Suppress "folder already exists, install anyway?" — UserPlugins always
; exists for any REAPER user, the prompt would just be noise.
DirExistsWarning=no
DisableProgramGroupPage=yes
DisableReadyPage=no
DisableWelcomePage=no
LicenseFile=..\LICENSE
OutputDir={#MyOutputDir}
OutputBaseFilename={#MyOutputBaseFilename}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64
ArchitecturesAllowed=x64
UninstallDisplayName={#MyAppName} {#MyAppVersion}
UninstallDisplayIcon={app}\{#MyDllName}
ShowLanguageDialog=auto

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; The DLL itself. Placed in the chosen REAPER plugin folder.
Source: "payload\{#MyDllName}"; DestDir: "{app}"; Flags: ignoreversion
; Bundle license + readme alongside the DLL so users always have them at hand.
Source: "..\LICENSE";   DestDir: "{app}"; DestName: "HoloRoll-LICENSE.txt"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; DestName: "HoloRoll-README.md";   Flags: ignoreversion

; v0.12.0-alpha.3: bundled JSFX placeholder. The HoloRoll extension
; inserts this on a dedicated track to host per-bone motion envelopes.
; Lands in REAPER's Effects/HoloRoll/ folder, NOT next to the DLL
; (Effects/ and UserPlugins/ are sibling directories under REAPER's
; resource path). Resolved by DefaultEffectsDir() below.
Source: "payload\effects\HoloRoll\*"; DestDir: "{code:DefaultEffectsDir}\HoloRoll"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Remove the legacy DLL from the MVP era so users don't end up with two
; copies of the plugin loaded by REAPER.
Type: files; Name: "{app}\{#MyLegacyDllName}"

[Run]
Filename: "{#MyAppURL}"; Description: "Visit project page"; Flags: postinstall shellexec skipifsilent unchecked

[UninstallDelete]
; Bundled docs we placed next to the DLL.
Type: files; Name: "{app}\HoloRoll-LICENSE.txt"
Type: files; Name: "{app}\HoloRoll-README.md"
; v0.12.0-alpha.3: clean up bundled JSFX folder on uninstall. The folder
; itself is removed if empty after files are gone. We don't touch user-
; created JSFX in REAPER's main Effects/ folder — only our own subfolder.
Type: filesandordirs; Name: "{code:DefaultEffectsDir}\HoloRoll"

[Code]

// --- Default plugin folder discovery ---------------------------------------
//
// Order:
//   1) Portable REAPER: registry hint HKCU\Software\Cockos\REAPER, key
//      "reaperinipath" -> portable folder. UserPlugins is a child.
//   2) Standard install: %APPDATA%\REAPER\UserPlugins.
function DefaultPluginsDir(Param: string): string;
var
  PortableRoot: string;
  Candidate: string;
begin
  Result := '';

  if RegQueryStringValue(HKCU, 'Software\Cockos\REAPER', 'reaperinipath', PortableRoot) then
  begin
    if PortableRoot <> '' then
    begin
      Candidate := AddBackslash(PortableRoot) + 'UserPlugins';
      if DirExists(Candidate) then
      begin
        Result := Candidate;
        Exit;
      end;
    end;
  end;

  Result := ExpandConstant('{userappdata}\REAPER\UserPlugins');
end;

// --- Default Effects folder discovery --------------------------------------
//
// Same logic as DefaultPluginsDir but resolves to REAPER's Effects/
// directory (sibling of UserPlugins). JSFX go here. We don't probe for
// existence on the portable path — if portable REAPER is detected, we
// always use <portable>/Effects/ (it's created automatically by REAPER
// the first time a user inserts a JSFX, but we may need to create it
// ourselves on first install).
function DefaultEffectsDir(Param: string): string;
var
  PortableRoot: string;
  Candidate: string;
begin
  Result := '';

  if RegQueryStringValue(HKCU, 'Software\Cockos\REAPER', 'reaperinipath', PortableRoot) then
  begin
    if PortableRoot <> '' then
    begin
      Candidate := AddBackslash(PortableRoot) + 'Effects';
      // For portable installs we trust the registry hint even if Effects/
      // doesn't exist yet — [Files] with createallsubdirs will materialise it.
      Result := Candidate;
      Exit;
    end;
  end;

  Result := ExpandConstant('{userappdata}\REAPER\Effects');
end;

// --- REAPER process detection / shutdown -----------------------------------

function IsReaperRunning(): Boolean;
begin
  Result := FindWindowByClassName('REAPERwnd') <> 0;
  if not Result then
    Result := FindWindowByClassName('REAPERwndPopup') <> 0;
end;

function TerminateReaper(): Boolean;
var
  ResultCode: Integer;
begin
  // /F = force, /IM = image name. Suppress output via > nul.
  Result := Exec(ExpandConstant('{cmd}'), '/C taskkill /F /IM reaper.exe > nul 2>&1',
                 '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // taskkill returns 0 on success, 128 if process was not running.
  if Result then
    Result := (ResultCode = 0) or (ResultCode = 128);
  // Tiny pause to let the OS release the DLL handle.
  if Result then
    Sleep(500);
end;

// --- PrepareToInstall: kill REAPER if needed (UI mode only) ----------------
//
// Returning a non-empty string from here causes Inno to abort with exit
// code 3 (prepare-to-install failed).
//
// alpha.12 behaviour split:
//   - SILENT / VERYSILENT mode: do nothing. Files are copied as-is. The
//     intended caller is either (a) a first-time install where REAPER
//     hasn't been opened yet, so file replacement succeeds cleanly, or
//     (b) an in-app auto-updater that takes care of the locked-DLL
//     problem on its own (e.g. by staging the new DLL into a sidecar
//     path and asking REAPER to swap on next start). Pre-alpha.12 the
//     installer used to taskkill /F reaper.exe in silent mode — that
//     destroyed unsaved work and was the wrong default for callers who
//     prefer to manage REAPER lifecycle themselves. If files happen to
//     be locked, Inno's standard "file in use" handling fires.
//   - UI mode (interactive double-click): unchanged. We still prompt
//     the user, taskkill on confirmation, abort with exit code 3 on
//     refusal. This keeps the click-through experience friendly.

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Response: Integer;
begin
  Result := '';
  NeedsRestart := False;

  // Silent / very-silent callers manage REAPER lifecycle themselves.
  // See block comment above for the rationale.
  if WizardSilent then
    Exit;

  if not IsReaperRunning() then
    Exit;

  Response := MsgBox(
    'REAPER is currently running.' + #13#10#13#10 +
    'The installer needs to close REAPER to replace the plugin DLL. ' +
    'Save your work, then click Yes to close REAPER and continue.',
    mbConfirmation, MB_YESNO);

  if Response = IDYES then
  begin
    if not TerminateReaper() then
      Result := 'Failed to close REAPER. Please close it manually and retry.';
  end
  else
  begin
    Result := 'Installation cancelled - REAPER must be closed first.';
  end;
end;
