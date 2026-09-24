@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-OfflineBots.ps1"
exit /b %ERRORLEVEL%
