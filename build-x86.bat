@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if errorlevel 1 (echo vcvars32 failed & exit /b 1)
pushd "%~dp0"
if not exist build\x86 mkdir build\x86
pushd build\x86
echo === TSF x86 ===
rc /nologo /fo wubi_tsf32.res ..\..\src\tsf\wubi_tsf.rc
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /LD /D_USRDLL /Fe:panda_tsf32.dll ..\..\src\engine\wubi_engine.cpp ..\..\src\engine\wubi_config.cpp ..\..\src\engine\wubi_composer.cpp ..\..\src\engine\wubi_english.cpp ..\..\src\tsf\wubi_tsf.cpp ..\..\src\tsf\wubi_langbar.cpp ..\..\src\tsf\wubi_bar.cpp /link /DEF:..\..\src\tsf\wubi_tsf.def /SUBSYSTEM:WINDOWS user32.lib gdi32.lib msimg32.lib gdiplus.lib advapi32.lib ole32.lib oleaut32.lib shell32.lib uuid.lib
set RC=%errorlevel%
popd
popd
exit /b %RC%