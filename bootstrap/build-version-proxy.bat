@echo off
setlocal

if "%VSCMD_ARG_TGT_ARCH%"=="" (
  echo Run this from an x64 Visual Studio developer environment.
  exit /b 2
)

if not exist build mkdir build
cl /nologo /std:c++17 /O2 /EHsc /LD /W4 /DUNICODE /D_UNICODE ^
  /Fe:build\version.dll VersionProxy.cpp ^
  /link /DEF:VersionProxy.def /OUT:build\version.dll User32.lib
exit /b %ERRORLEVEL%
