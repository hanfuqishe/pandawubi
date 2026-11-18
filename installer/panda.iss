; panda.iss —— 熊猫五笔的 Inno Setup 脚本
; 由 tools\build-setup.ps1 调用 ISCC 编译：d:\...\ISCC.exe /DMyVersion=1.0.0 installer\panda.iss
; 用到的源文件都在 ..\dist 和 ..\data 下（构建脚本会先确保它们是最新的）。

#ifndef MyVersion
  #define MyVersion "1.0.0"
#endif
#ifndef MyRoot
  #define MyRoot ".."
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

[Setup]
AppId={{8F3C1A52-7B4E-4C6A-9D21-5E7A0C3B19D4}
AppName=熊猫五笔
AppVerName=熊猫五笔 {#MyVersion}
AppVersion={#MyVersion}
AppPublisher=hanfuqishe
AppPublisherURL=https://github.com/hanfuqishe/pandawubi
AppSupportURL=https://github.com/hanfuqishe/pandawubi
DefaultDirName={autopf}\PandaWubi
DefaultGroupName=熊猫五笔
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=PandaWubi-Setup-{#MyVersion}
SetupIconFile={#MyRoot}\dist\panda.ico
UninstallDisplayIcon={app}\panda.ico
UninstallDisplayName=熊猫五笔
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
MinVersion=10.0
CloseApplications=no
RestartApplications=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
SetupAppTitle=熊猫五笔 安装程序
SetupWindowTitle=安装 熊猫五笔 {#MyVersion}
WelcomeLabel1=欢迎安装 [name]
WelcomeLabel2=这会把熊猫五笔 {#MyVersion} 装到你的电脑上。%n%n装完还需要在「设置 > 时间和语言 > 语言和区域 > 中文(简体) > 语言选项」里点「添加键盘 > 熊猫五笔」，然后用 Win+空格 切换过来。
SelectDirLabel3=安装程序会把熊猫五笔装到下面这个文件夹。
SelectDirBrowseLabel=点「下一步」继续；要换目录就点「浏览」。
ReadyLabel1=准备安装
ReadyLabel2a=点「安装」开始，或点「上一步」再改改设置。
InstallingLabel=正在安装熊猫五笔，请稍候…
FinishedHeadingLabel=熊猫五笔 安装完成
FinishedLabel=熊猫五笔已经装好了。%n%n最后一步：设置 > 时间和语言 > 语言和区域 > 中文(简体) > 语言选项 > 添加键盘 > 熊猫五笔。%n然后在任意输入框按 Win+空格 切过来。
FinishedLabelNoIcons=熊猫五笔已经装好了。%n%n最后一步：设置 > 时间和语言 > 语言和区域 > 中文(简体) > 语言选项 > 添加键盘 > 熊猫五笔。%n然后在任意输入框按 Win+空格 切过来。
ButtonNext=下一步(&N) >
ButtonBack=< 上一步(&B)
ButtonInstall=安装(&I)
ButtonFinish=完成(&F)
ButtonCancel=取消(&C)
ButtonBrowse=浏览(&B)…
ExitSetupMessage=安装还没完成。真要退出吗？
ConfirmUninstall=确定要卸载 熊猫五笔 吗？
UninstalledAll=熊猫五笔 已卸载。%n%n你的配置和用户词还留在 %APPDATA%\PandaWubi，需要的话可以手动删掉。
StatusUninstalling=正在卸载 熊猫五笔…

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式（打开设置）"; GroupDescription: "附加任务:"; Flags: unchecked

[Files]
Source: "{#MyRoot}\dist\panda_tsf.dll";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyRoot}\dist\panda_tsf32.dll";      DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyRoot}\dist\panda-settings.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyRoot}\dist\panda-phrase.exe";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyRoot}\data\qqwubi86.dict.tsv"; DestDir: "{app}\data"; Flags: ignoreversion
Source: "{#MyRoot}\data\simp2trad.dat";        DestDir: "{app}\data"; Flags: ignoreversion
Source: "{#MyRoot}\data\punc.ini";             DestDir: "{app}\data"; Flags: ignoreversion
Source: "{#MyRoot}\data\eng_words.txt";        DestDir: "{app}\data"; Flags: ignoreversion
; 四个主题图标：一份放程序目录，一份放 %LOCALAPPDATA%\PandaWubi（输入指示器只认那里）
Source: "{#MyRoot}\dist\panda.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyRoot}\dist\cat.ico";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyRoot}\dist\dog.ico";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyRoot}\dist\crane.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyRoot}\dist\panda.ico"; DestDir: "{localappdata}\PandaWubi"; Flags: ignoreversion
Source: "{#MyRoot}\dist\cat.ico";   DestDir: "{localappdata}\PandaWubi"; Flags: ignoreversion
Source: "{#MyRoot}\dist\dog.ico";   DestDir: "{localappdata}\PandaWubi"; Flags: ignoreversion
Source: "{#MyRoot}\dist\crane.ico"; DestDir: "{localappdata}\PandaWubi"; Flags: ignoreversion

[Icons]
Name: "{group}\熊猫五笔 设置"; Filename: "{app}\panda-settings.exe"
Name: "{group}\卸载 熊猫五笔"; Filename: "{uninstallexe}"
Name: "{autodesktop}\熊猫五笔 设置"; Filename: "{app}\panda-settings.exe"; Tasks: desktopicon

[Registry]
; 输入法在语言列表里的显示信息（DllRegisterServer 已经写了 CLSID/CTF，这里补 Description/图标/启用）
Root: HKLM; Subkey: "SOFTWARE\Microsoft\CTF\TIP\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}\LanguageProfile\0x00000804\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}"; ValueType: string; ValueName: "Description"; ValueData: "熊猫五笔"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Microsoft\CTF\TIP\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}\LanguageProfile\0x00000804\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}"; ValueType: string; ValueName: "IconFile"; ValueData: "{localappdata}\PandaWubi\panda.ico"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Microsoft\CTF\TIP\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}\LanguageProfile\0x00000804\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}"; ValueType: dword;  ValueName: "IconIndex"; ValueData: "0"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Microsoft\CTF\TIP\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}\LanguageProfile\0x00000804\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}"; ValueType: dword;  ValueName: "Enable"; ValueData: "1"; Flags: uninsdeletevalue

; 同一组值再写一份到 HKCU：DLL 运行时也是写 HKCU 的，两处一致才不会被覆盖成别的路径
Root: HKCU; Subkey: "Software\Microsoft\CTF\TIP\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}\LanguageProfile\0x00000804\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}"; ValueType: string; ValueName: "Description"; ValueData: "熊猫五笔"
Root: HKCU; Subkey: "Software\Microsoft\CTF\TIP\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}\LanguageProfile\0x00000804\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}"; ValueType: string; ValueName: "IconFile"; ValueData: "{localappdata}\PandaWubi\panda.ico"
Root: HKCU; Subkey: "Software\Microsoft\CTF\TIP\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}\LanguageProfile\0x00000804\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}"; ValueType: dword;  ValueName: "IconIndex"; ValueData: "0"
Root: HKCU; Subkey: "Software\Microsoft\CTF\TIP\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}\LanguageProfile\0x00000804\{{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}"; ValueType: dword;  ValueName: "Enable"; ValueData: "1"

[Run]
; 64 位和 32 位各注册一次；DllRegisterServer 写的是 HKLM 的 CLSID + CTF TIP
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\panda_tsf.dll"""; StatusMsg: "正在注册 64 位输入法…"; Flags: runhidden waituntilterminated
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s ""{app}\panda_tsf32.dll"""; StatusMsg: "正在注册 32 位输入法…"; Flags: runhidden waituntilterminated; Check: IsWin64
; 首次安装时生成一份默认配置（已有配置就跳过，设置程序只会补缺失项）
Filename: "{app}\panda-settings.exe"; Parameters: "/set CandCount=9"; StatusMsg: "写入初始配置…"; Flags: runhidden waituntilterminated; Check: not FileExists(ExpandConstant('{userappdata}\PandaWubi\config.ini'))
Filename: "{app}\panda-settings.exe"; Description: "打开设置"; Flags: postinstall nowait skipifsilent unchecked

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\panda_tsf.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregTsf64"
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/s /u ""{app}\panda_tsf32.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregTsf32"; Check: IsWin64

[UninstallDelete]
; 用户配置（%APPDATA%\PandaWubi）故意保留
Type: filesandordirs; Name: "{app}\data"
Type: dirifempty; Name: "{app}"
Type: files; Name: "{localappdata}\PandaWubi\panda.ico"
Type: files; Name: "{localappdata}\PandaWubi\cat.ico"
Type: files; Name: "{localappdata}\PandaWubi\dog.ico"
Type: files; Name: "{localappdata}\PandaWubi\crane.ico"


