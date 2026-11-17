@echo off
setlocal
net session >nul 2>&1
if not "%errorlevel%"=="0" (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set SELF=%~dp0
regsvr32.exe /s /u "%SELF%panda_tsf.dll"
"%WINDIR%\SysWOW64\regsvr32.exe" /s /u "%SELF%panda_tsf32.dll"
echo Done.
pause