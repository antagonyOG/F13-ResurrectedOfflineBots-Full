@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-OfflineBots.ps1"
exit /b %ERRORLEVEL%
