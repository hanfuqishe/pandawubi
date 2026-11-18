# build-setup.ps1 —— 打包安装程序 dist\PandaWubi-Setup-<版本>.exe
#
# 做法：把要安装的文件全部编成 RCDATA 资源塞进 src\ui\panda-setup.cpp 编出来的 exe 里，
# 于是得到一个真正"单文件"的安装包（不需要 Inno/NSIS，机器上只要 VS 编译器）。
#
# 用法: powershell -ExecutionPolicy Bypass -File .\tools\build-setup.ps1 [-SkipBuild]
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# ---- 版本号 ----
$vh = Get-Content (Join-Path $root 'src\tsf\panda_version.h') -Raw
if ($vh -notmatch 'PANDA_WUBI_VERSION\s+L"([^"]+)"') { throw '读不到版本号' }
$version = $matches[1]
"[1/5] 版本 $version"

# ---- 先确保 dist 里是最新的构建产物 ----
if (-not $SkipBuild) {
    "[2/5] 重新构建（build.bat + build-x86.bat）"
    & cmd /c "`"$root\build.bat`"" *> "$env:TEMP\panda-setup-build.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$env:TEMP\panda-setup-build.log" -Tail 20; throw 'build.bat 失败' }
    & cmd /c "`"$root\build-x86.bat`"" *> "$env:TEMP\panda-setup-buildx86.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$env:TEMP\panda-setup-buildx86.log" -Tail 20; throw 'build-x86.bat 失败' }
    foreach ($f in @('panda_tsf.dll', 'x86\panda_tsf32.dll', 'panda-settings.exe', 'panda-phrase.exe')) {
        $src = Join-Path $root "build\$f"
        $dst = if ($f -like 'x86\*') { Join-Path $root "dist\panda_tsf32.dll" } else { Join-Path $root "dist\$f" }
        if (-not (Test-Path $src)) { throw "缺少构建产物 $src" }
        Copy-Item $src $dst -Force
    }
} else {
    "[2/5] 跳过构建（-SkipBuild）"
}

# ---- 收集载荷 ----
$stage = Join-Path $root 'build\setup-stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage 'data') | Out-Null

$payload = [ordered]@{
    200 = @{ src = 'dist\panda_tsf.dll';            dst = 'panda_tsf.dll' }
    201 = @{ src = 'dist\panda_tsf32.dll';          dst = 'panda_tsf32.dll' }
    202 = @{ src = 'dist\panda-settings.exe';       dst = 'panda-settings.exe' }
    203 = @{ src = 'dist\panda-phrase.exe';         dst = 'panda-phrase.exe' }
    204 = @{ src = 'dist\panda.ico';                dst = 'panda.ico' }
    205 = @{ src = 'dist\cat.ico';                  dst = 'cat.ico' }
    206 = @{ src = 'dist\dog.ico';                  dst = 'dog.ico' }
    207 = @{ src = 'dist\crane.ico';                dst = 'crane.ico' }
    208 = @{ src = 'data\qqwubi86.dict.tsv';        dst = 'data\qqwubi86.dict.tsv' }
    209 = @{ src = 'data\simp2trad.dat';            dst = 'data\simp2trad.dat' }
    210 = @{ src = 'data\punc.ini';                 dst = 'data\punc.ini' }
    211 = @{ src = 'data\eng_words.txt';            dst = 'data\eng_words.txt' }
}

"[3/5] 收集载荷"
$total = 0
foreach ($kv in $payload.GetEnumerator()) {
    $e = $kv.Value
    $src = Join-Path $root $e.src
    if (-not (Test-Path $src)) { throw "缺少文件 $($e.src)" }
    $dst = Join-Path $stage $e.dst
    $dir = Split-Path -Parent $dst
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Copy-Item $src $dst -Force
    $total += (Get-Item $src).Length
}

# readme
$readme = @"
熊猫五笔 v$version  ——  TSF 五笔输入法

用法
  1) 在「设置 > 时间和语言 > 语言和区域 > 中文(简体) > 语言选项」里
     点「添加键盘」→「熊猫五笔」；
  2) 任意输入框按 Win+空格 切到熊猫五笔；
  3) 悬浮栏上的齿轮可以打开设置窗口。

常用
  Ctrl+Shift+T   简繁切换       Ctrl+Shift+P   中文标点开关
  Ctrl+=         加自定义词       Ctrl+Shift+=   自定义短语
  Z              最近上屏的 4 个词 + 版本号
  ;              便捷输入

目录
  程序：%ProgramFiles%\PandaWubi（data 子目录放词库）
  配置与用户词：%APPDATA%\PandaWubi（config.ini、user.tsv）

卸载：控制面板「应用」里找「熊猫五笔」，或运行安装目录下的 panda-setup.exe /uninstall
"@
Set-Content -Path (Join-Path $stage 'readme.txt') -Value $readme -Encoding UTF8

# ---- 生成 rc + manifest ----
$rc = New-Object System.Text.StringBuilder
[void]$rc.AppendLine('#include <windows.h>')
[void]$rc.AppendLine('1 RT_MANIFEST "setup.manifest"')
foreach ($kv in $payload.GetEnumerator()) {
    [void]$rc.AppendLine("$($kv.Key) RCDATA `"$($kv.Value.dst.Replace('\','\\'))`"")
}
[void]$rc.AppendLine('212 RCDATA "readme.txt"')
Set-Content -Path (Join-Path $stage 'setup.rc') -Value $rc.ToString() -Encoding ASCII

$manifest = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity version="1.0.0.0" processorArchitecture="*" name="PandaWubi.Setup" type="win32"/>
  <description>熊猫五笔 安装程序</description>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="requireAdministrator" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
  <dependency>
    <dependentAssembly>
      <assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls"
                        version="6.0.0.0" processorArchitecture="*"
                        publicKeyToken="6595b64144ccf1df" language="*"/>
    </dependentAssembly>
  </dependency>
</assembly>
'@
Set-Content -Path (Join-Path $stage 'setup.manifest') -Value $manifest -Encoding UTF8

# ---- 编译 ----
"[4/5] 编译安装程序"
$vs = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vs)) { throw "找不到 vcvars64.bat：$vs" }
$out = Join-Path $root "dist\PandaWubi-Setup-$version.exe"
$log = "$env:TEMP\panda-setup-cl.log"
# 写成 .bat 再执行：多行命令直接喂给 cmd /c 时续行符容易被吞掉
$bat = Join-Path $env:TEMP 'panda-setup-build.bat'
$lines = @(
    '@echo off',
    "call `"$vs`" >nul",
    "cd /d `"$stage`"",
    'rc /nologo /fo setup.res setup.rc',
    'if errorlevel 1 exit /b 1',
    "cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /DUNICODE /D_UNICODE /I `"$root\src\tsf`" /Fe:`"$out`" `"$root\src\ui\panda-setup.cpp`" setup.res /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib advapi32.lib shell32.lib ole32.lib"
)
Set-Content -Path $bat -Value $lines -Encoding ASCII
& cmd /c "`"$bat`"" *> $log
if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 25; throw '编译安装程序失败' }
$warn = Select-String -Path $log -Pattern 'warning C|error C|error LNK' -ErrorAction SilentlyContinue
if ($warn) { $warn | ForEach-Object { "  " + $_.Line.Trim() } | Select-Object -First 6 }

"[5/5] 完成"
$fi = Get-Item $out
"  输出: $out"
"  大小: {0:N1} MB（载荷 {1:N1} MB）" -f ($fi.Length / 1MB), ($total / 1MB)
