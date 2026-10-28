@echo off
rem 3D Braindance Edition: uninstall. Extra arguments (for example -Force) are passed on.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Braindance3D-Setup.ps1" -Action uninstall -NoPause %*
if errorlevel 1 echo.& echo Something went wrong. See the messages above and Braindance3D-setup.log.
pause
