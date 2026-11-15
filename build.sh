#!/bin/sh
set -eu
PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ ! -f "$PROJECT_DIR/HumiditySensor/ota_secrets.h" ]; then
  printf '%s\n' 'Run python3 setup_ota.py once to create the OTA password.' >&2
  exit 1
fi
# Use a local sketchbook so iCloud-only legacy libraries cannot stall builds.
export ARDUINO_DIRECTORIES_USER="$PROJECT_DIR/sketchbook"
exec arduino-cli compile \
  --fqbn 'esp8266:esp8266:generic:eesz=1M64,FlashMode=dout,FlashFreq=40,ResetMethod=ck' \
  --library "$PROJECT_DIR/vendor/WiFiManager-2.0.17" \
  --library "$PROJECT_DIR/vendor/DHT-sensor-library-1.4.6" \
  --library "$PROJECT_DIR/vendor/Adafruit_Sensor-1.1.15" \
  --build-path "$PROJECT_DIR/build-local" \
  "$PROJECT_DIR/HumiditySensor"
