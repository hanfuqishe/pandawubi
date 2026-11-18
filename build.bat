@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo vcvars64 failed & exit /b 1)
pushd "%~dp0"
if not exist build mkdir build
pushd build
echo === CLI ===
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /Fe:panda-cli.exe ..\src\engine\wubi_engine.cpp ..\src\engine\wubi_config.cpp ..\src\engine\wubi_composer.cpp ..\src\engine\wubi_english.cpp ..\src\cli\wubi-cli.cpp /link advapi32.lib
if errorlevel 1 (popd & popd & exit /b 1)
echo === TSF x64 ===
rc /nologo /fo panda_tsf.res ..\src\tsf\wubi_tsf.rc
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /LD /D_USRDLL /Fe:panda_tsf.dll ..\src\engine\wubi_engine.cpp ..\src\engine\wubi_config.cpp ..\src\engine\wubi_composer.cpp ..\src\engine\wubi_english.cpp ..\src\tsf\wubi_tsf.cpp ..\src\tsf\wubi_langbar.cpp ..\src\tsf\wubi_bar.cpp panda_tsf.res /link /DEF:..\src\tsf\wubi_tsf.def /SUBSYSTEM:WINDOWS user32.lib gdi32.lib msimg32.lib gdiplus.lib advapi32.lib ole32.lib oleaut32.lib shell32.lib uuid.lib
if errorlevel 1 (popd & popd & exit /b 1)
echo === selftest ===
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /Fe:tsf-selftest.exe ..\src\cli\tsf-selftest.cpp /link ole32.lib oleaut32.lib uuid.lib advapi32.lib
set RC=%errorlevel%
echo === bar singleton test ===
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /Fe:bar-singleton-test.exe ..\src\cli\bar-singleton-test.cpp /link ole32.lib oleaut32.lib uuid.lib user32.lib
if errorlevel 1 set RC=1
popd
popd
if not "%RC%"=="0" (exit /b %RC%)
echo === tsf-host ===
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /Fe:"%~dp0build\tsf-host.exe" "%~dp0src\cli\tsf-host.cpp" /link user32.lib gdi32.lib msimg32.lib gdiplus.lib ole32.lib oleaut32.lib uuid.lib
echo === settings ===
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /DUNICODE /D_UNICODE /Fe:"%~dp0build\panda-settings.exe" "%~dp0src\ui\wubi-settings.cpp" "%~dp0src\engine\wubi_config.cpp" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib advapi32.lib shell32.lib
echo === phrase window ===
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /DUNICODE /D_UNICODE /Fe:"%~dp0build\panda-phrase.exe" "%~dp0src\ui\ppwubi-phrase.cpp" "%~dp0src\engine\wubi_config.cpp" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib advapi32.lib imm32.lib

echo === qq-oracle ===
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /Fe:"%~dp0build\qq-oracle.exe" "%~dp0tools\qq-oracle.cpp" /link user32.lib imm32.lib gdi32.lib msimg32.lib gdiplus.lib
call "%~dp0build-x86.bat"
exit /b %errorlevel%