@echo off
setlocal enabledelayedexpansion

rem Builds BlockTestCpp.sln from the command line, no IDE required.
rem Usage:
rem   build.bat            (builds Debug and Release, x64)
rem   build.bat Debug      (builds Debug only, x64)
rem   build.bat Release    (builds Release only, x64)
rem   build.bat clean      (removes bin\ and obj\)

set "SOLUTION=%~dp0BlockTestCpp.sln"
set "PLATFORM=x64"
set "CONFIG=%~1"

if /i "%CONFIG%"=="clean" (
    echo Cleaning bin\ and obj\ ...
    if exist "%~dp0bin" rmdir /s /q "%~dp0bin"
    if exist "%~dp0obj" rmdir /s /q "%~dp0obj"
    exit /b 0
)

rem Locate vswhere (ships with VS 2017+) to find the newest MSBuild.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Could not find vswhere.exe. Is Visual Studio installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
    set "MSBUILD=%%i"
)

if not defined MSBUILD (
    echo ERROR: Could not locate MSBuild.exe via vswhere. Ensure the "Desktop development with C++" workload is installed.
    exit /b 1
)

if not "%CONFIG%"=="" (
    echo Building %CONFIG%^|%PLATFORM% with "%MSBUILD%" ...
    "%MSBUILD%" "%SOLUTION%" /m /nologo /verbosity:minimal /p:Configuration=%CONFIG% /p:Platform=%PLATFORM%
    exit /b %ERRORLEVEL%
)

echo Building Debug^|%PLATFORM% with "%MSBUILD%" ...
"%MSBUILD%" "%SOLUTION%" /m /nologo /verbosity:minimal /p:Configuration=Debug /p:Platform=%PLATFORM%
if errorlevel 1 exit /b %ERRORLEVEL%

echo Building Release^|%PLATFORM% with "%MSBUILD%" ...
"%MSBUILD%" "%SOLUTION%" /m /nologo /verbosity:minimal /p:Configuration=Release /p:Platform=%PLATFORM%
if errorlevel 1 exit /b %ERRORLEVEL%

echo.
echo Build succeeded. Binaries in bin\%PLATFORM%\Debug\ and bin\%PLATFORM%\Release\
exit /b 0
