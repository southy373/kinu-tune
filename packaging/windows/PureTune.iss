; Windows installer for Pure Tune (the script can still build the upstream
; Kinu Tune product when passed /DProductName="Kinu Tune").
; Each product installs its models into its own folder:
; %PROGRAMDATA%\Keinoaza\<ProductName>\models. Pure Tune no longer shares
; the Kinu Tune folder, and the old shared folder is never touched here.

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
  #define ProductName "Pure Tune"
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
; Models live in the product's own folder. Pure Tune keeps them across an
; uninstall unless the user opts into deleting them at uninstall time
; (see the [Code] prompt below), so the flag stays on for that product.
#if ProductName == "Pure Tune"
Source: "{#StageDir}\models\*"; DestDir: "{commonappdata}\Keinoaza\{#ProductName}\models"; Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall
#else
Source: "{#StageDir}\models\*"; DestDir: "{commonappdata}\Keinoaza\{#ProductName}\models"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\MODIFICATIONS.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Redist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "{cm:InstallingRedist}"; Flags: waituntilterminated

#if ProductName == "Pure Tune"
[Code]
// Pure Tune owns its model and settings folders outright (they are no
// longer shared with Kinu Tune), so an uninstall may offer to remove
// them too. The default is No: a plain uninstall keeps models and
// settings for the next install. Note that {userappdata} resolves to
// the profile of the user running the (elevated) uninstaller.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ModelsDir: String;
  SettingsDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    ModelsDir := ExpandConstant('{commonappdata}\Keinoaza\Pure Tune\models');
    SettingsDir := ExpandConstant('{userappdata}\Pure Tune');
    if MsgBox('Also delete Pure Tune''s model files and settings?' + #13#10 +
              'Models: ' + ModelsDir + #13#10 +
              'Settings and logs: ' + SettingsDir + #13#10#13#10 +
              'Pure Tune のモデルファイルと設定も削除しますか？',
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
    begin
      DelTree(ModelsDir, True, True, True);
      DelTree(SettingsDir, True, True, True);
    end;
  end;
end;
#endif
