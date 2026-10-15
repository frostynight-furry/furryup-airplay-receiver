@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 (
    echo ERROR: Cannot enter the source directory.
    exit /b 1
)
REM ============================================
REM AirPlay Live Source - Windows Build Script
REM ============================================
REM
REM Prerequisites:
REM   1. Visual Studio 2022 or newer with the C++ desktop workload
REM   2. CMake 3.16+
REM   3. scripts\prepare-deps.ps1 completed successfully
REM
REM This build intentionally uses only the locked dependencies under deps.
REM It does not download packages or fall back to machine-wide SDKs.
REM ============================================

where cl.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: MSVC cl.exe is not available.
    echo Run this script from an x64 Visual Studio Developer Command Prompt.
    popd
    endlocal
    exit /b 1
)
where nmake.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: nmake.exe is not available.
    popd
    endlocal
    exit /b 1
)
where cmake.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake.exe is not available. Install the Visual Studio CMake component.
    popd
    endlocal
    exit /b 1
)
if /I not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo ERROR: This build requires an x64 Visual Studio Developer Command Prompt.
    echo Current VSCMD_ARG_TGT_ARCH: %VSCMD_ARG_TGT_ARCH%
    popd
    endlocal
    exit /b 1
)

set "LOCAL_OPENSSL_DIR=%CD%\deps\openssl"
set "LOCAL_FFMPEG_ROOT=%CD%\deps\ffmpeg-sdk"
set "MISSING_DEPS=0"
call :require_file "deps\prepared-dependencies.json"
call :require_file "deps\obs-sdk\obs-studio-30.0.0\libobs\obs.h"
call :require_file "deps\obs-sdk\obs-studio-30.0.0\libobs\obsconfig.h"
call :require_file "deps\obs-sdk\obs-studio-30.0.0\deps\w32-pthreads\pthread.h"
call :require_file "deps\ffmpeg-sdk\include\libavcodec\avcodec.h"
call :require_file "deps\ffmpeg-sdk\lib\avcodec.lib"
call :require_file "deps\openssl\include\openssl\opensslv.h"
call :require_file "deps\openssl\lib\libcrypto.lib"
call :require_file "deps\openssl\bin\libcrypto-3-x64.dll"
call :require_file "deps\libplist-2.7.0\build\plist.lib"
call :require_file "deps\UxPlay\lib\raop.h"
call :require_file "deps\uxplay-build\build\airplay.lib"
call :require_file "deps\uxplay-build\build\playfair.lib"
call :require_file "deps\uxplay-build\build\llhttp.lib"
call :require_file "deps\obs.lib"
call :require_file "deps\w32-pthreads.lib"
if "%MISSING_DEPS%"=="1" (
    echo.
    echo ERROR: Local build dependencies are incomplete.
    echo Run scripts\prepare-deps.ps1 from this x64 Developer Command Prompt.
    popd
    endlocal
    exit /b 1
)
echo Using locked FFmpeg SDK: %LOCAL_FFMPEG_ROOT%
echo Using locked OpenSSL:   %LOCAL_OPENSSL_DIR%

REM Use a dedicated local NMake tree so it cannot collide with CI or a
REM previous Visual Studio multi-config generator cache.
if not exist build-local mkdir build-local
if not exist build-local (
    echo ERROR: Cannot create the build directory.
    popd
    endlocal
    exit /b 1
)
cd /d build-local
if errorlevel 1 (
    echo ERROR: Cannot enter the build directory.
    popd
    endlocal
    exit /b 1
)

REM Configure with CMake
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl ^
    -DFFMPEG_ROOT="%LOCAL_FFMPEG_ROOT%" ^
    -DOPENSSL_DIR="%LOCAL_OPENSSL_DIR%"

if errorlevel 1 (
    echo.
    echo CMake configuration failed!
    echo Make sure the dependencies in deps\ are prepared correctly.
    popd
    endlocal
    exit /b 1
)

REM Build
cmake --build .

if errorlevel 1 (
    echo.
    echo Build failed!
    popd
    endlocal
    exit /b 1
)

echo.
echo ============================================
echo Build successful!
echo.
echo Output:
echo   build-local\airplay-live-source.dll
echo.
echo Do not copy only the DLL. A runnable package also needs the matching
echo OpenSSL runtime, locale files, notices, conflict checks, and firewall
echo rules. Use the complete internal-alpha artifact/installer described in
echo README.md.
echo ============================================

popd
endlocal
exit /b 0

:require_file
if not exist "%~1" (
    echo ERROR: Missing dependency file: %~1
    set "MISSING_DEPS=1"
)
exit /b 0
