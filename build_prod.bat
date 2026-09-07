@echo off
rem ============================================================
rem  AirNode PRODUCTION build (test tools trimmed)
rem  One-click build of the production firmware:
rem    - CONFIG_AIRNODE_TEST_TOOLS_ENABLE=n  (from sdkconfig.prod)
rem    - isolated output dir  build_prod\
rem    - your dev sdkconfig and build\ are NOT touched
rem
rem  Usage:
rem    double-click, or run from a terminal:  build_prod.bat
rem    flash result with:  idf.py -B build_prod -p COMx flash
rem ============================================================
setlocal
cd /d "%~dp0"

rem ---- ESP-IDF location (edit if installed elsewhere) ----
if not defined IDF_PATH set "IDF_PATH=D:\work\esp32_idf\esp-idf"

rem ---- always activate: export.bat sets IDF_PYTHON_ENV_PATH etc.
rem      (idf.py may exist on PATH but python env is missing without it) ----
echo [env] Activating ESP-IDF from %IDF_PATH% ...
call "%IDF_PATH%\export.bat"
if errorlevel 1 (
    echo [ERROR] ESP-IDF activation failed.
    exit /b 1
)

echo [build] Building AirNode PRODUCTION firmware (test tools trimmed)...
idf.py -DSDKCONFIG="%~dp0build_prod\sdkconfig" -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.prod" -B build_prod build
if errorlevel 1 (
    echo.
    echo [FAILED] Build failed, see messages above.
    exit /b 1
)

echo.
echo [OK] Production firmware:  %~dp0build_prod\hello_world.bin
echo     Flash it with:  idf.py -B build_prod -p COMx flash
endlocal
exit /b 0
