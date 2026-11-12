@echo off
rem build OWLOS Flight Radar firmware with ESP-IDF 5.5 (same install as for ViewOwl)
rem   BUILD.BAT                   build for the 320x240 CYD
rem   BUILD.BAT COM4              build, flash to COM4, open serial monitor (opening the port resets the board)
rem   BUILD.BAT st7796            build for another display
rem   BUILD.BAT st7796 COM4       same, and flash it
rem displays: ili9341 (2.8" CYD, all three revisions, the default)  st7796 (3.5" CYD)
rem           ili9486 (RPi LCD on OWLOSAirQuality)  gc9a01 (round 1.28", ESP32-C3)
rem           es3c28p (LCDWIKI ES3C28P 2.8", ESP32-S3)  nmcydc5 (NM-CYD-C5 2.8", ESP32-C5)
rem           crowadv28 (Elecrow CrowPanel Advance 2.8", ESP32-S3)
rem           crowadv50 (Elecrow CrowPanel Advance 5.0", ESP32-S3)  jc8048w550 (Guition 5", ESP32-S3)
rem           tab5 (M5Stack Tab5 5", ESP32-P4)  esp4827s043 (ESP32-4827S043C 4.3", ESP32-S3)
rem           crow70 (Elecrow CrowPanel 7.0", ESP32-S3)
rem           wave7 (Waveshare ESP32-S3-Touch-LCD-7)  wave7b (Waveshare ESP32-S3-Touch-LCD-7B, 1024x600)
rem           epd400 (4.2" e-paper, S3)  epd792 (5.79" e-paper, S3)
rem each display gets its own build folder and sdkconfig, so switching doesnt rebuild everything
setlocal
set DISP=%~1
set PORT=%~2
if /i "%DISP:~0,3%"=="COM" (
  set PORT=%DISP%
  set DISP=
)
if "%DISP%"=="" set DISP=ili9341
set NUM=
if /i "%DISP%"=="ili9341" (set NUM=1& set TARGET=esp32)
if /i "%DISP%"=="st7796"  (set NUM=2& set TARGET=esp32)
if /i "%DISP%"=="ili9486" (set NUM=3& set TARGET=esp32)
if /i "%DISP%"=="gc9a01"  (set NUM=4& set TARGET=esp32c3)
if /i "%DISP%"=="epd400"  (set NUM=5& set TARGET=esp32s3)
if /i "%DISP%"=="epd792"  (set NUM=6& set TARGET=esp32s3)
if /i "%DISP%"=="es3c28p" (set NUM=7& set TARGET=esp32s3)
if /i "%DISP%"=="nmcydc5" (set NUM=8& set TARGET=esp32c5)
if /i "%DISP%"=="crowadv28" (set NUM=9& set TARGET=esp32s3)
if /i "%DISP%"=="crowadv50" (set NUM=10& set TARGET=esp32s3)
if /i "%DISP%"=="jc8048w550" (set NUM=11& set TARGET=esp32s3)
if /i "%DISP%"=="tab5" (set NUM=12& set TARGET=esp32p4)
if /i "%DISP%"=="esp4827s043" (set NUM=13& set TARGET=esp32s3)
if /i "%DISP%"=="crow70" (set NUM=14& set TARGET=esp32s3)
if /i "%DISP%"=="wave7" (set NUM=15& set TARGET=esp32s3)
if /i "%DISP%"=="wave7b" (set NUM=16& set TARGET=esp32s3)
if "%NUM%"=="" (
  echo unknown display %DISP%, pick one of: ili9341 st7796 ili9486 gc9a01 es3c28p nmcydc5 crowadv28 crowadv50 jc8048w550 tab5 esp4827s043 crow70 wave7 wave7b epd400 epd792
  exit /b 1
)
rem the CYD keeps the plain build folder and sdkconfig it always had
set BDIR=build
set SDK=%~dp0sdkconfig
if not "%NUM%"=="1" (
  set BDIR=build_%DISP%
  set SDK=%~dp0build_%DISP%\sdkconfig
)

if "%IDF_PATH%"=="" set "IDF_PATH=%USERPROFILE%\esp\v5.5\esp-idf"
if "%IDF_TOOLS_PATH%"=="" set "IDF_TOOLS_PATH=%USERPROFILE%\.espressif"
if not exist "%IDF_PATH%\export.bat" (
  echo ESP-IDF not found at %IDF_PATH%
  exit /b 1
)
rem IDF venv was made with IDF's own python, not the system one, so put that first in PATH
for /d %%P in ("%IDF_TOOLS_PATH%\tools\idf-python\*") do set "PATH=%%P;%PATH%"
call "%IDF_PATH%\export.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0"
rem a build folder made where the project used to be (it was moved or copied) keeps the
rem old path in its CMake cache and CMake refuses it. such a folder is started fresh
set "HERE=%~dp0"
set "HERE=%HERE:\=/%"
if exist "%BDIR%\CMakeCache.txt" (
  findstr /i /c:"CMAKE_HOME_DIRECTORY:INTERNAL=%HERE:~0,-1%" "%BDIR%\CMakeCache.txt" >nul
  if errorlevel 1 (
    echo %BDIR% was made in another folder, starting it fresh
    rmdir /s /q "%BDIR%"
  )
)
set ARGS=-B %BDIR% "-DSDKCONFIG=%SDK%" -DIDF_TARGET=%TARGET% -DRADAR_DISPLAY=%NUM%
rem ESP-IDF 5.5.0 still calls the C5 a preview target, later 5.5 releases (the site's build) do not.
rem the flag lets 5.5.0 build it and changes nothing for the other chips
set PREVIEW=
if /i "%TARGET%"=="esp32c5" set PREVIEW=--preview
if "%PORT%"=="" (
  idf.py %PREVIEW% %ARGS% build
) else (
  idf.py %PREVIEW% %ARGS% -p %PORT% build flash monitor
)
