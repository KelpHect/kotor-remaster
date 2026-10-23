@echo off
rem Lane 08 build environment: Visual Studio 2026 Build Tools (MSVC 14.51),
rem bundled CMake 4.3.1 and Ninja 1.13.2. This only affects the calling cmd
rem process; use it via "call scripts\vs-env.bat" then run cmake.
rem Override the toolchain location with KOTOR_VS_PATH if needed.

if defined KOTOR_VS_PATH (
    set "VSB=%KOTOR_VS_PATH%"
) else (
    set "VSB=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
)

if not exist "%VSB%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [ERROR] vcvars64.bat not found under "%VSB%".
    echo         Set KOTOR_VS_PATH to your Visual Studio Build Tools installation.
    exit /b 1
)

if not defined VSCMD_ARG_TGT_ARCH (
    call "%VSB%\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 exit /b 1
)

set "PATH=%VSB%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSB%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
exit /b 0
