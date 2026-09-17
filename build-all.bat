@echo off
setlocal EnableExtensions
call "%~dp0build-backend.bat"
if errorlevel 1 exit /b 1
call "%~dp0build-loader.bat"
if errorlevel 1 exit /b 1
echo.
echo Full packed loader and backend built successfully.
exit /b 0
