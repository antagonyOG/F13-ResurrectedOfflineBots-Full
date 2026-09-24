@echo off
setlocal

call "%~dp0VERIFY-FROZEN-HASHES.bat"
if errorlevel 1 (
  echo.
  echo Build stopped so the frozen autonomous Jason AI cannot be altered accidentally.
  exit /b 1
)
set Path=
set "Path=%SystemRoot%\System32;%SystemRoot%"
set "TEMP=%LOCALAPPDATA%\Temp"
set "TMP=%LOCALAPPDATA%\Temp"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio with Desktop development with C++ is required.
  exit /b 1
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
  echo Visual Studio not found.
  exit /b 1
)
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" exit /b 2
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "MSBUILD=%VSROOT%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" exit /b 2
"%MSBUILD%" "%~dp0ResurrectedOfflineBots.sln" /m /p:Configuration=Release /p:Platform=x64 /p:TrackFileAccess=false
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
copy /y "%~dp0bin\ResurrectedOfflineBots.dll" "%~dp0ResurrectedOfflineBots.dll" >nul

echo.
echo Built: %~dp0ResurrectedOfflineBots.dll
echo Frozen Features/Engine hashes were verified before compilation.
echo Runtime behavior must be verified on the target game installation.
