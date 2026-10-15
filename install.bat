@echo off
setlocal
REM ============================================
REM AirPlay Live Source - Installer
REM Run as Administrator
REM ============================================

set "PLUGIN_ROOT=%PROGRAMDATA%\obs-studio\plugins\airplay-live-source"
set "PLUGIN_DIR=%PLUGIN_ROOT%\bin\64bit"
set "LOCALE_DIR=%PLUGIN_ROOT%\data\locale"
set "SCRIPT_DIR=%~dp0"
set "NETSH_EXE=%SystemRoot%\System32\netsh.exe"
set "POWERSHELL_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"

echo.
echo AirPlay Live Source - Installer
echo ================================
echo.

if exist "%PROGRAMDATA%\obs-studio\plugins\obs-airplay-receiver" (
    echo ERROR: The upstream OBS AirPlay Receiver plugin is installed.
    echo Uninstall it first; both plugins use the same AirPlay ports.
    pause
    exit /b 1
)

if not exist "%NETSH_EXE%" (
    echo ERROR: Windows netsh.exe was not found at %NETSH_EXE%
    pause
    exit /b 1
)
if not exist "%POWERSHELL_EXE%" (
    echo ERROR: Windows PowerShell was not found at %POWERSHELL_EXE%
    pause
    exit /b 1
)

REM This alpha deliberately reuses the exact FFmpeg runtime shipped by the
REM registered OBS 32.2.2 installation. Reject a different or incomplete host
REM before changing any plugin files.
"%POWERSHELL_EXE%" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
    "$ErrorActionPreference='Stop'; $keys=@('HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio','HKCU:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio','HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio','HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio'); $app=$null; foreach($key in $keys){if(Test-Path -LiteralPath $key){$app=Get-ItemProperty -LiteralPath $key; break}}; if($null -eq $app){throw 'Registered OBS Studio installation not found'}; if($app.DisplayVersion -ne '32.2.2'){throw ('This alpha requires OBS Studio 32.2.2; found ' + $app.DisplayVersion)}; $obsExe=([string]$app.DisplayIcon -replace ',[0-9]+$','').Trim([char]34); if(-not (Test-Path -LiteralPath $obsExe -PathType Leaf)){throw 'Registered OBS executable not found'}; $obsBin=Split-Path -Parent $obsExe; $expected=@{'obs64.exe'='32.2.2.0';'avcodec-62.dll'='62.28.102.0';'avutil-60.dll'='60.26.102.0';'swscale-9.dll'='9.5.102.0';'swresample-6.dll'='6.3.102.0'}; foreach($name in $expected.Keys){$path=Join-Path $obsBin $name; if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw ('Missing OBS host file: ' + $name)}; $actual=(Get-Item -LiteralPath $path).VersionInfo.FileVersionRaw.ToString(); if($actual -ne $expected[$name]){throw ('Unexpected ' + $name + ' version: ' + $actual)}}"
if errorlevel 1 (
    echo.
    echo ERROR: OBS host compatibility validation failed. No files were changed.
    pause
    exit /b 1
)

REM Validate the complete artifact before changing the installed plugin.
for %%F in (airplay-live-source.dll ffmpeg-sdk-buildconf.txt README.md UPSTREAM-LGPL-NOTICE.txt GPL-3.0.txt LGPL-2.1-or-later.txt OPENSSL-LICENSE.txt NOTICE.md RELEASE-GATE.md THIRD_PARTY_NOTICES.md) do (
    if not exist "%SCRIPT_DIR%%%F" (
        echo ERROR: Missing required artifact file %%F
        pause
        exit /b 1
    )
)
if not exist "%SCRIPT_DIR%data\locale\en-US.ini" (
    echo ERROR: Missing English locale file
    pause
    exit /b 1
)
if not exist "%SCRIPT_DIR%data\locale\zh-CN.ini" (
    echo ERROR: Missing Chinese locale file
    pause
    exit /b 1
)
if not exist "%SCRIPT_DIR%libcrypto-3*-x64.dll" (
    echo ERROR: Missing required OpenSSL 3 x64 runtime DLL
    pause
    exit /b 1
)

REM Create plugin directory
if not exist "%PLUGIN_DIR%" (
    echo Creating directory: %PLUGIN_DIR%
    mkdir "%PLUGIN_DIR%"
    if errorlevel 1 (
        echo.
        echo ERROR: Failed to create directory. Please run as Administrator.
        pause
        exit /b 1
    )
)

if not exist "%LOCALE_DIR%" mkdir "%LOCALE_DIR%"

REM Remove duplicate FFmpeg DLLs from earlier alpha packages. The current
REM plugin resolves these exact module names from the OBS 32.2.2 host.
for %%F in (avcodec-62.dll avutil-60.dll swscale-9.dll swresample-6.dll) do (
    if exist "%PLUGIN_DIR%\%%F" del /q "%PLUGIN_DIR%\%%F"
    if exist "%PLUGIN_DIR%\%%F" (
        echo ERROR: Failed to remove obsolete %%F. Close OBS and retry.
        pause
        exit /b 1
    )
)

REM Copy plugin DLL
echo Copying airplay-live-source.dll...
copy /y "%SCRIPT_DIR%airplay-live-source.dll" "%PLUGIN_DIR%\" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy airplay-live-source.dll
    pause
    exit /b 1
)

echo Copying translations...
xcopy /y /q "%SCRIPT_DIR%data\locale\*.ini" "%LOCALE_DIR%\" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy locale files
    pause
    exit /b 1
)

for %%F in (ffmpeg-sdk-buildconf.txt README.md UPSTREAM-LGPL-NOTICE.txt GPL-3.0.txt LGPL-2.1-or-later.txt OPENSSL-LICENSE.txt NOTICE.md RELEASE-GATE.md THIRD_PARTY_NOTICES.md) do (
    copy /y "%SCRIPT_DIR%%%F" "%PLUGIN_ROOT%\" >nul
    if errorlevel 1 (
        echo ERROR: Failed to copy %%F
        pause
        exit /b 1
    )
)

REM Copy the OpenSSL 3 x64 dependency used by the pinned UxPlay build.
echo Copying libcrypto DLL...
copy /y "%SCRIPT_DIR%libcrypto-3*-x64.dll" "%PLUGIN_DIR%\" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy libcrypto DLL
    pause
    exit /b 1
)

REM Open firewall for AirPlay (audio arrives as inbound UDP)
echo Adding firewall rules...
set "FIREWALL_OK=1"
"%NETSH_EXE%" advfirewall firewall delete rule name="AirPlay Live Source (UDP)" >nul 2>&1
"%NETSH_EXE%" advfirewall firewall delete rule name="AirPlay Live Source (TCP)" >nul 2>&1
"%NETSH_EXE%" advfirewall firewall add rule name="AirPlay Live Source (UDP)" dir=in action=allow protocol=UDP localport=6000-6001,7011 profile=private >nul
if errorlevel 1 set "FIREWALL_OK=0"
"%NETSH_EXE%" advfirewall firewall add rule name="AirPlay Live Source (TCP)" dir=in action=allow protocol=TCP localport=7000,7100 profile=private >nul
if errorlevel 1 set "FIREWALL_OK=0"

if "%FIREWALL_OK%"=="0" goto firewall_failed

echo.
echo ============================================
echo Installation successful!
echo.
echo Plugin installed to:
echo   %PLUGIN_DIR%
echo.
echo Close uxplay-windows before using this plugin.
echo Restart OBS Studio, then add a new source:
echo   Sources ^> + ^> iPhone Screen Receiver (AirPlay)
echo ============================================
echo.
pause
exit /b 0

:firewall_failed
echo.
echo WARNING: Plugin files were copied, but firewall rules could not be added.
echo Re-run this installer as Administrator before testing AirPlay streaming.
echo.
pause
exit /b 2
