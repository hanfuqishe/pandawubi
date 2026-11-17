@echo off
setlocal
net session >nul 2>&1
if not "%errorlevel%"=="0" (
  echo Requesting administrator privileges...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%1' -Verb RunAs"
  exit /b
)
set SELF=%~dp0
set CLSID={7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}
set PROFILE=HKLM\SOFTWARE\Microsoft\CTF\TIP\%CLSID%\LanguageProfile\0x00000804\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A12}
set MODE=%1
if "%MODE%"=="" set MODE=file

if /i "%MODE%"=="file" (
  set ICON=%LOCALAPPDATA%\PandaWubi\panda.ico
  set IDX=0
) else if /i "%MODE%"=="dir" (
  set ICON=%SELF%panda.ico
  set IDX=0
) else if /i "%MODE%"=="dll" (
  set ICON=%SELF%panda_tsf.dll
  set IDX=0
) else if /i "%MODE%"=="dllneg" (
  set ICON=%SELF%panda_tsf.dll
  set IDX=-1
) else (
  echo usage: seticon.cmd [file^|dir^|dll^|dllneg]
  pause
  exit /b 1
)

reg add "%PROFILE%" /v IconFile  /t REG_SZ    /d "%ICON%" /f >nul
reg add "%PROFILE%" /v IconIndex /t REG_DWORD /d %IDX%    /f >nul
echo.
echo   mode   = %MODE%
echo   IconFile = %ICON%
echo   Index    = %IDX%
echo.
echo Now restart Explorer (Task Manager -^> Windows Explorer -^> Restart) and check the indicator.
pause