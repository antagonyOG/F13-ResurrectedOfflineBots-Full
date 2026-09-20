@echo off
setlocal

call "%~dp0VERIFY-FROZEN-HASHES.bat"
if errorlevel 1 (
  echo.
  echo Build stopped so the frozen autonomous Jason AI cannot be altered accidentally.
  pause
  exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio 2022 or Build Tools with Desktop development with C++ is required.
  pause
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
  echo MSBuild not found.
  pause
  exit /b 1
)
"%MSBUILD%" "%~dp0ResurrectedOfflineBots.sln" /m /p:Configuration=Release /p:Platform=x64 /p:TrackFileAccess=false
if errorlevel 1 (
  echo Build failed.
  pause
  exit /b 1
)
copy /y "%~dp0bin\ResurrectedOfflineBots.dll" "%~dp0ResurrectedOfflineBots.dll" >nul

echo.
echo Built: %~dp0ResurrectedOfflineBots.dll
echo 18L-AC hooks ILLBackendBlueprintLibrary::RequestOfflineMode synchronously and uses PASS-THROUGH lifecycle trace hooks only.
echo No lifecycle behavior is replaced and frozen AI is not auto-started in this foundation proof.
echo Frozen Features/Engine hashes were verified before compilation.
pause
