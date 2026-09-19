@echo off
setlocal enabledelayedexpansion

set "SKETCH=D:\Fox_ESP32_FW"
set "LOGDIR=D:\Fox_ESP32_FW\Compilers\logs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
for /f "delims=" %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "TIMESTAMP=%%i"
set "LOG=%LOGDIR%\classic_%TIMESTAMP%.log"
set "ARDUINO_CLI="

REM ================================================================
REM Fox_ESP32_FW - Compile Classic - ESP32 Dev Module ONLY
REM Part of the Fox_ESP32_FW Compilers set. Each run writes its OWN
REM timestamped log file into Compilers\logs\ instead of overwriting
REM a single shared log, so every past attempt stays on record.
REM
REM Wipes both Arduino's global "cores" cache and the per-sketch build
REM cache before compiling, so this is always a genuine from-scratch
REM rebuild - this project hit a real arduino-cli bug once where the
REM global cores cache silently reused a stale core compiled under a
REM DIFFERENT board-menu setting than the FQBN actually requested, so
REM every script in this set forces a real cold rebuild rather than
REM trusting arduino-cli's own cache invalidation. This makes each run
REM take a few minutes even for a single board - by design, not a bug.
REM ================================================================

REM --- locate arduino-cli.exe (bundled with Arduino IDE 2.x, or on PATH) ---
if exist "%LocalAppData%\Programs\arduino-ide\resources\app\lib\backend\resources\arduino-cli.exe" (
    set "ARDUINO_CLI=%LocalAppData%\Programs\arduino-ide\resources\app\lib\backend\resources\arduino-cli.exe"
) else if exist "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" (
    set "ARDUINO_CLI=C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
) else (
    where arduino-cli >nul 2>nul
    if !errorlevel! == 0 set "ARDUINO_CLI=arduino-cli"
)

if "%ARDUINO_CLI%"=="" (
    echo [FATAL] Could not find arduino-cli.exe in the usual Arduino IDE 2.x install locations or on PATH.
    echo Edit the ARDUINO_CLI paths near the top of this .bat to point at it directly
    echo ^(Help -^> About in the Arduino IDE, or search your install folder for arduino-cli.exe^),
    echo then re-run this file.
    echo.
    echo [FATAL] arduino-cli.exe not found - see message above. > "%LOG%"
    pause
    exit /b 1
)

echo ================================================================> "%LOG%"
echo Fox_ESP32_FW - Compile Classic - ESP32 Dev Module ONLY>> "%LOG%"
echo Started: %DATE% %TIME%>> "%LOG%"
echo Using arduino-cli: %ARDUINO_CLI%>> "%LOG%"
echo ================================================================>> "%LOG%"

echo ================================================================
echo Fox_ESP32_FW - Compile Classic - ESP32 Dev Module ONLY
echo Started: %DATE% %TIME%
echo Using arduino-cli: %ARDUINO_CLI%
echo Log file: %LOG%
echo ================================================================
echo.
echo This window will print live progress as it compiles - a few
echo minutes of compiler output is normal. It will NOT close itself
echo when done; press any key at the end to close it.
echo.

call :log "Wiping global core cache and per-sketch build cache for a true from-scratch build..."
if exist "C:\Users\PC\AppData\Local\arduino\cores" (
    rd /s /q "C:\Users\PC\AppData\Local\arduino\cores"
    call :log "  - cleared cores cache"
)
if exist "C:\Users\PC\AppData\Local\arduino\sketches" (
    rd /s /q "C:\Users\PC\AppData\Local\arduino\sketches"
    call :log "  - cleared per-sketch build cache"
)
call :log "Cache cleared. Starting compile - this recompiles fully from scratch, so expect this to take a few minutes."

call :compile_board "Classic - ESP32 Dev Module" "esp32:esp32:esp32:UploadSpeed=921600,FlashMode=qio,FlashSize=4M,PartitionScheme=huge_app,DebugLevel=none,PSRAM=disabled"

call :log ""
call :log "================================================================"
call :log "DONE - Finished: %DATE% %TIME%"
call :log "Scroll up (or check this log file) for any [FAILED] lines."
call :log "================================================================"
echo.
echo Done. Press any key to close this window.
pause >nul
exit /b 0

:compile_board
set "BOARD_NAME=%~1"
set "FQBN=%~2"
call :log ""
call :log "----------------------------------------------------------------"
call :log "[START] !BOARD_NAME! - %TIME%"
call :log "FQBN: !FQBN!"
call :log "----------------------------------------------------------------"
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%ARDUINO_CLI%' compile --fqbn '!FQBN!' --export-binaries '%SKETCH%' 2>&1 | Tee-Object -FilePath '%LOG%' -Append; exit $LASTEXITCODE"
if !errorlevel! == 0 (
    call :log "[OK] !BOARD_NAME! compiled successfully - %TIME%"
) else (
    call :log "[FAILED] !BOARD_NAME! - exit code !errorlevel! - %TIME%"
)
goto :eof

:log
echo %~1
echo %~1>> "%LOG%"
goto :eof
