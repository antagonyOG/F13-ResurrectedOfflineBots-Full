@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Verify-OfflineBots.ps1"
exit /b %ERRORLEVEL%
