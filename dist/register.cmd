@echo off
setlocal enabledelayedexpansion
net session >nul 2>&1
if not "%errorlevel%"=="0" (
  echo Requesting administrator privileges...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set SELF=%~dp0
set CLSID={7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}
set TIPKEY=HKLM\SOFTWARE\Microsoft\CTF\TIP\%CLSID%
echo.
echo === 1/3 64-bit ===
regsvr32.exe /s "%SELF%panda_tsf.dll"
echo   regsvr32 x64 exit=%errorlevel%
echo === 2/3 32-bit ===
"%WINDIR%\SysWOW64\regsvr32.exe" /s "%SELF%panda_tsf32.dll"
echo   regsvr32 x86 exit=%errorlevel%
rem ---- 显式写一次语言配置：两次 regsvr32 谁最后跑都会覆盖 IconFile，这里强制纠正 ----
set PROFILEKEY=HKLM\SOFTWARE\Microsoft\CTF\TIP\%CLSID%\LanguageProfile\0x00000804\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}
reg add "%PROFILEKEY%" /v Description /t REG_SZ /d "Panda五笔" /f >nul
rem 图标放到用户目录：实测深层路径（D:\...\dist）在输入指示器上读不到，用户目录可以
if not exist "%LOCALAPPDATA%\PandaWubi" mkdir "%LOCALAPPDATA%\PandaWubi"
copy /y "%SELF%panda.ico" "%LOCALAPPDATA%\PandaWubi\panda.ico" >nul
attrib +R "%LOCALAPPDATA%\PandaWubi\panda.ico" >nul 2>&1
reg add "%PROFILEKEY%" /v IconFile    /t REG_SZ /d "%LOCALAPPDATA%\PandaWubi\panda.ico" /f >nul
reg add "%PROFILEKEY%" /v IconIndex   /t REG_DWORD /d 0 /f >nul
reg add "%PROFILEKEY%" /v Enable      /t REG_DWORD /d 1 /f >nul
echo   [OK] profile icon -> %SELF%panda.ico
echo === 3/3 verify ===
reg query "%TIPKEY%" /s >nul 2>&1
if errorlevel 1 (
  echo   [FAIL] HKLM TIP key NOT created - registration did not take effect.
  goto :diag
)
reg query "HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32" >nul 2>&1
if errorlevel 1 (
  echo   [FAIL] HKLM CLSID InprocServer32 NOT created.
  goto :diag
)
echo   [OK] HKLM TIP + CLSID created.
if not exist "%APPDATA%\PandaWubi" mkdir "%APPDATA%\PandaWubi"
if not exist "%APPDATA%\PandaWubi\config.ini" copy /y "%SELF%config.ini" "%APPDATA%\PandaWubi\config.ini" >nul
echo.
echo DONE. Next steps:
echo   Settings ^> Time ^& language ^> Language ^& region ^> Chinese ^(Simplified^)
echo     ^> Language options ^> Add a keyboard ^> Panda五笔
echo   Then focus Notepad and press Win+Space.
echo.
echo Tip: type "a" - you should see a candidate window; Space commits the first candidate.
pause
exit /b 0

:diag
echo.
echo --- diagnosis ---
echo Log files under %%LOCALAPPDATA%%\WubiTsf :
if exist "%LOCALAPPDATA%\PandaWubi" (
  for %%f in ("%LOCALAPPDATA%\PandaWubi\*.log") do (
    echo --- %%~nxf ---
    type "%%f"
  )
) else (
  echo   ^(no log directory - the DLL never ran^)
)
echo.
echo Possible causes:
echo   * UAC prompt was cancelled  -^> right-click this file, Run as administrator
echo   * Antivirus blocked regsvr32 -^> check AV logs
echo   * DLL missing/corrupt -^> re-copy panda_tsf.dll into this folder
pause
exit /b 1