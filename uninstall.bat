@echo off
setlocal
REM ============================================
REM AirPlay Live Source - Uninstaller
REM Run as Administrator
REM ============================================

set "PLUGIN_DIR=%PROGRAMDATA%\obs-studio\plugins\airplay-live-source"
set "NETSH_EXE=%SystemRoot%\System32\netsh.exe"

echo.
echo AirPlay Live Source - Uninstaller
echo ==================================
echo.

if exist "%PLUGIN_DIR%" (
    echo Removing: %PLUGIN_DIR%
    rmdir /s /q "%PLUGIN_DIR%"
    if errorlevel 1 (
        echo.
        echo ERROR: Failed to remove plugin directory. Please run as Administrator.
        echo Make sure OBS Studio is closed.
        pause
        exit /b 1
    )
) else (
    echo Plugin directory not found; continuing with firewall cleanup.
)

echo Removing firewall rules...
set "FIREWALL_OK=1"
if not exist "%NETSH_EXE%" set "FIREWALL_OK=0"
if exist "%NETSH_EXE%" (
    "%NETSH_EXE%" advfirewall firewall delete rule name="AirPlay Live Source (UDP)" >nul 2>&1
    if errorlevel 1 set "FIREWALL_OK=0"
    "%NETSH_EXE%" advfirewall firewall delete rule name="AirPlay Live Source (TCP)" >nul 2>&1
    if errorlevel 1 set "FIREWALL_OK=0"
)

if "%FIREWALL_OK%"=="0" goto firewall_failed

echo.
echo ============================================
echo Cleanup successful!
echo.
echo The plugin files and firewall rules have been removed.
echo Restart OBS Studio for changes to take effect.
echo ============================================
echo.
pause
exit /b 0

:firewall_failed
echo.
echo WARNING: Plugin files were removed, but firewall rule cleanup failed.
echo Re-run this uninstaller as Administrator or remove the two
echo "AirPlay Live Source" firewall rules manually.
echo.
pause
exit /b 2
