; Windows installer for Kinu Tune and the separately installable Pure Tune build.
; Models intentionally remain in the shared folder read by the current plug-in:
; %PROGRAMDATA%\Keinoaza\Kinu Tune\models.

#ifndef AppVersion
  #error Pass /DAppVersion=<version>
#endif
#ifndef StageDir
  #error Pass /DStageDir=<unpacked release folder>
#endif
#ifndef Redist
  #error Pass /DRedist=<path to vc_redist.x64.exe>
#endif
#ifndef ProductName
  #define ProductName "Kinu Tune"
#endif
#ifndef ProductAppId
  #define ProductAppId "38373E75-C136-4F15-8F5D-6F966750C257"
#endif

[Setup]
; Kinu Tune keeps its existing AppId; Pure Tune has a separate identity.
#if ProductName == "Pure Tune"
AppId={#ProductAppId}
#else
AppId={{38373E75-C136-4F15-8F5D-6F966750C257}
#endif
AppName={#ProductName}
AppVersion={#AppVersion}
AppVerName={#ProductName} {#AppVersion}
AppPublisher=Keinoaza
AppPublisherURL=https://github.com/keinoaza666-stack/kinu-tune
AppSupportURL=https://github.com/keinoaza666-stack/kinu-tune/issues
DefaultDirName={commonpf64}\Keinoaza\{#ProductName}
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=Output
OutputBaseFilename={#StringChange(ProductName, " ", "-")}-{#AppVersion}-Windows-x64-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage
UninstallDisplayName={#ProductName}
UninstallDisplayIcon={uninstallexe}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Messages]
english.FinishedLabelNoIcons={#ProductName} is installed. Restart your DAW and rescan plug-ins.
japanese.FinishedLabelNoIcons={#ProductName} をインストールしました。DAW を起動し直し、プラグインを再スキャンしてください。

[CustomMessages]
english.InstallingRedist=Installing the Microsoft Visual C++ Redistributable...
japanese.InstallingRedist=Microsoft Visual C++ 再頒布可能パッケージをインストールしています...

[InstallDelete]
; Remove only this product's bundle, so Kinu Tune and Pure Tune coexist.
Type: filesandordirs; Name: "{commoncf64}\VST3\{#ProductName}.vst3"

[Files]
Source: "{#StageDir}\{#ProductName}.vst3\*"; DestDir: "{commoncf64}\VST3\{#ProductName}.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
; The current Pure Tune build reads the Kinu Tune model folder too. Preserve
; shared files when either product is uninstalled.
#if ProductName == "Pure Tune"
Source: "{#StageDir}\models\*"; DestDir: "{commonappdata}\Keinoaza\Kinu Tune\models"; Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall
#else
Source: "{#StageDir}\models\*"; DestDir: "{commonappdata}\Keinoaza\Kinu Tune\models"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\MODIFICATIONS.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Redist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "{cm:InstallingRedist}"; Flags: waituntilterminated
