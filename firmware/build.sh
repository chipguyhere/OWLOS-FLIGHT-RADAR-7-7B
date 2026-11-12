#!/bin/sh
# macOS / Linux counterpart of BUILD.BAT for the boards this fork adds (and any other, by number):
#   ./build.sh wave7b                     build for the Waveshare ESP32-S3-Touch-LCD-7B
#   ./build.sh wave7b /dev/cu.usbserial-X build, flash, open the serial monitor
# needs ESP-IDF 5.5 ($IDF_PATH, or ~/esp/esp-idf, ~/espidf5.5/esp-idf)
set -e
DISP=${1:-wave7b}
PORT=$2
case "$DISP" in
  wave7)  NUM=15 ;;
  wave7b) NUM=16 ;;
  crow70) NUM=14 ;;
  *) echo "this script knows wave7 wave7b crow70; the rest: BUILD.BAT"; exit 1 ;;
esac
if [ -z "$IDF_PATH" ]; then
  for d in "$HOME/esp/esp-idf" "$HOME/espidf5.5/esp-idf"; do [ -f "$d/export.sh" ] && IDF_PATH=$d && break; done
fi
[ -f "$IDF_PATH/export.sh" ] || { echo "ESP-IDF not found, set IDF_PATH"; exit 1; }
. "$IDF_PATH/export.sh" >/dev/null
cd "$(dirname "$0")"
ARGS="-B build_$DISP -DSDKCONFIG=build_$DISP/sdkconfig -DIDF_TARGET=esp32s3 -DRADAR_DISPLAY=$NUM"
if [ -z "$PORT" ]; then idf.py $ARGS build; else idf.py $ARGS -p "$PORT" build flash monitor; fi
