@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0backend\src\Game"
set "FAIL=0"

call :CHECK "%ROOT%\Features\Features.cpp" 1fb08fef18980d0d65e6392caad439b03387ceefbaf2e1f08944e742a6f315bf
call :CHECK "%ROOT%\Features\Features.hpp" 09d29bfb90c142d3668fc52fb215879fd383670d2eb97bf0decf08789a3d032b
call :CHECK "%ROOT%\Engine\Engine.cpp" 2b9abdecdf2e70f5b96606c86165a0393c7357553640df57d7d4829c556156e6
call :CHECK "%ROOT%\Engine\Engine.hpp" abe325645d21cf048178bb555c64310430b60a0650c6fd14691f8e8f84454c8c

echo.
if "%FAIL%"=="0" (
  echo FROZEN AI/ENGINE HASHES VERIFIED.
  exit /b 0
)

echo ERROR: A frozen AI/Engine file changed. Backend build blocked.
exit /b 1

:CHECK
set "FILE=%~1"
set "EXPECTED=%~2"
if not exist "%FILE%" (
  echo MISSING: %FILE%
  set "FAIL=1"
  exit /b 0
)
set "ACTUAL="
for /f "tokens=1" %%H in ('%SystemRoot%\System32\certutil.exe -hashfile "%FILE%" SHA256 ^| %SystemRoot%\System32\findstr.exe /R /V "hash CertUtil"') do if not defined ACTUAL set "ACTUAL=%%H"
if /I "!ACTUAL!"=="%EXPECTED%" (
  echo OK: %~nx1
) else (
  echo BAD: %~nx1
  echo   expected %EXPECTED%
  echo   actual   !ACTUAL!
  set "FAIL=1"
)
exit /b 0
