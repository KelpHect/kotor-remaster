@echo off
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=msvc-release"

call "%~dp0vs-env.bat"
if errorlevel 1 exit /b 1

cd /d "%~dp0.."
echo [configure] preset=%PRESET%
cmake --preset "%PRESET%"
exit /b %errorlevel%
