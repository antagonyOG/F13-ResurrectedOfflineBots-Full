@echo off
setlocal EnableExtensions
cd /d "%~dp0"
rem Some launchers supply both Path and PATH; MSBuild rejects that environment.
set Path=
set "Path=%SystemRoot%\System32;%SystemRoot%"
set "TEMP=%LOCALAPPDATA%\Temp"
set "TMP=%LOCALAPPDATA%\Temp"
call "%~dp0VERIFY-FROZEN-HASHES.bat"
if errorlevel 1 exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: Visual Studio Installer\vswhere.exe not found.
  exit /b 2
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -find MSBuild\Current\Bin\MSBuild.exe`) do set "MSBUILD=%%I"
if not defined MSBUILD (
  echo ERROR: MSBuild not found. Install Visual Studio with Desktop development with C++.
  exit /b 2
)
"%MSBUILD%" "%~dp0ResurrectedOfflineBots.sln" /m:1 /nologo /v:minimal /p:Configuration=Release /p:Platform=x64 /p:TrackFileAccess=false
if errorlevel 1 exit /b 1
echo Built backend: %~dp0bin\ResurrectedOfflineBots.dll
exit /b 0
