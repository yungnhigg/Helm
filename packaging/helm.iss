; Helm installer — Inno Setup 6
;
; Produces a single Helm-Setup-<version>.exe with Start Menu entry, optional
; desktop shortcut, and an uninstaller.
;
; This packages an already-built Release binary. It does not compile anything on
; the target machine, so run make_installer.cmd rather than compiling this
; script directly — that builds first and stages the files this script expects.
;
; Two things are deliberately NOT bundled:
;
;   Models. A GGUF is several gigabytes and which one you want is a choice, not
;   a default. Helm loads them from wherever you point it.
;
;   The external tool runtime. install_helm_tools.cmd pulls roughly 20 GB of
;   ComfyUI, SDXL, whisper.cpp, and Chromium. It ships inside the install so it
;   can be run afterwards, optionally kicked off by the last installer page.

#define AppName      "Helm"
#define AppPublisher "yungnhigg"
#define AppExeName   "Helm.exe"

; Version is read from the VERSION file at compile time so it never drifts from
; the source tree.
#define AppVersion   Trim(FileRead(FileOpen("..\VERSION")))

[Setup]
AppId={{8E2C4A17-6D3B-4F51-9A28-7C1E5B0D9F44}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
OutputDir=..\dist
OutputBaseFilename=Helm-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; x64 only. The build is 64-bit and llama.cpp with CUDA has no 32-bit path.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Program Files needs elevation. All mutable state lives in %LOCALAPPDATA%\Helm,
; so the installed tree itself stays read-only in normal use.
PrivilegesRequired=admin
DisableProgramGroupPage=yes
LicenseFile=
SetupIconFile=

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "runtools"; Description: "Install the external tool runtime after setup (~20 GB download, 30-60 minutes)"; GroupDescription: "Optional components:"; Flags: unchecked

[Files]
; The executable and everything the post-build step copies beside it.
Source: "..\build\Release\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\Release\web\*";           DestDir: "{app}\web";           Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\build\Release\config\*";        DestDir: "{app}\config";        Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\build\Release\tools_runtime\*"; DestDir: "{app}\tools_runtime"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\install_helm_tools.cmd";        DestDir: "{app}";               Flags: ignoreversion
Source: "..\README.md";                     DestDir: "{app}";               Flags: ignoreversion isreadme

; CUDA runtime libraries.
;
; llama.cpp is linked statically, but the CUDA runtime and cuBLAS are not — they
; are always dynamic. Without these three DLLs the executable fails to start on
; any machine that has not installed the CUDA Toolkit, with a missing-DLL dialog
; that names cudart and explains nothing.
;
; make_installer.cmd stages them into staging\cuda so this script does not have
; to guess a toolkit version or install path.
Source: "staging\cuda\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#AppName}";              Filename: "{app}\{#AppExeName}"
Name: "{group}\Install Helm tools";      Filename: "{app}\install_helm_tools.cmd"
Name: "{group}\Uninstall {#AppName}";    Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";        Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
; The tool installer is a console script that wants to be watched — it downloads
; for the better part of an hour and prints per-step progress. Shown, not hidden.
Filename: "{app}\install_helm_tools.cmd"; Description: "Installing external tools"; Tasks: runtools; Flags: postinstall skipifsilent
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
; Leaves %LOCALAPPDATA%\Helm alone: sessions, memory.md, and app.json are the
; user's, not the installer's. Removing them silently on uninstall would destroy
; conversation history and a hand-curated memory file.
Type: filesandordirs; Name: "{app}\web"
Type: filesandordirs; Name: "{app}\tools_runtime"

[Code]
function InitializeSetup(): Boolean;
var
  Version: String;
begin
  Result := True;
  // WebView2 is the entire interface layer. It ships with Windows 11 and recent
  // Windows 10, but a machine without it launches to a blank window with no
  // error, which is a miserable thing to debug. Check rather than assume.
  if not (RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version)
       or RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', Version)) then
  begin
    if MsgBox('The WebView2 runtime was not detected. Helm''s interface will not render without it.' + #13#10#13#10 +
              'Install it from https://developer.microsoft.com/microsoft-edge/webview2/ and run setup again.' + #13#10#13#10 +
              'Continue anyway?', mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
  end;
end;
