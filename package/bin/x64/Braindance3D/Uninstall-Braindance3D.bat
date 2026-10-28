@echo off
setlocal
rem 3D Braindance Edition: uninstall. Extra arguments are passed on.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Braindance3D-Setup.ps1" -Action uninstall -NoPause %*
set "BD3D_RC=%ERRORLEVEL%"
if not "%BD3D_RC%"=="0" echo.& echo Something went wrong, exit code %BD3D_RC%. See the messages above and Braindance3D-setup.log.
pause
exit /b %BD3D_RC%
