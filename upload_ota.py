#!/usr/bin/env python3
"""Upload built firmware over the LAN without putting its password in argv."""
import argparse
import hashlib
import importlib.util
import json
import logging
from pathlib import Path
import secrets
import socket
import sys
import urllib.request


def main():
    project = Path(__file__).resolve().parent
    config_path = project / "secrets" / "device.json"
    config = json.loads(config_path.read_text()) if config_path.exists() else {}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=config.get("host"), help="Sensor IP or .local hostname")
    parser.add_argument("--expected-hostname", default=config.get("hostname"), help="Optional exact device identity check")
    parser.add_argument("--espota", type=Path, help="Path to the ESP8266 core's espota.py")
    parser.add_argument("--preserve-history", action="store_true", help="Back up history before OTA and restore it after reboot")
    args = parser.parse_args()
    if not args.host:
        parser.error("Specify --host or save host/hostname in secrets/device.json.")
    image = project / "build-local" / "HumiditySensor.ino.bin"
    password_file = project / "secrets" / "ota-password.txt"
    if not image.is_file() or not password_file.is_file():
        parser.error("Run setup_ota.py and build.sh before uploading.")
    password = password_file.read_text().strip()
    if not password:
        parser.error("OTA password file is empty.")
    image_bytes = image.read_bytes()
    if hashlib.md5(password.encode()).hexdigest().encode() not in image_bytes:
        parser.error("Built firmware does not contain the saved password hash; rebuild before uploading.")
    if args.preserve_history and b'"history_restore":true' not in image_bytes:
        parser.error("This image does not advertise history restore; refusing to clear history for this upload.")
    remote_ip = socket.gethostbyname(args.host)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open("http://" + remote_ip + "/api/device", timeout=10) as response:
        device = json.load(response)
    if not device.get("firmware", "").startswith(("history-ota-", "history-uptime-")) or not device.get("ota_enabled"):
        parser.error("Target does not report compatible OTA-enabled humidity firmware.")
    if args.expected_hostname and device.get("hostname") != args.expected_hostname:
        parser.error("Target hostname does not match the expected sensor.")
    if image.stat().st_size > device["free_sketch_space"]:
        parser.error("Firmware is too large for the sensor's free OTA space.")
    roots = [Path.home() / "Library/Arduino15", Path.home() / ".arduino15",
             Path.home() / "AppData/Local/Arduino15"]
    candidates = [root / "packages/esp8266/hardware/esp8266/3.0.2/tools/espota.py" for root in roots]
    uploader = args.espota or next((path for path in candidates if path.is_file()), None)
    if not uploader or not uploader.is_file():
        parser.error("Install ESP8266 core 3.0.2 or specify --espota /path/to/espota.py.")
    spec = importlib.util.spec_from_file_location("espota", uploader)
    espota = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(espota)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
        route.connect((remote_ip, 8266))
        local_ip = route.getsockname()[0]
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    snapshot = None
    if args.preserve_history:
        from history_transfer import save_history
        snapshot = save_history("http://" + remote_ip, project / "exports")
    # Call the official uploader in-process so the password stays out of argv/logs.
    result = espota.serve(remote_ip, local_ip, 8266, 10000 + secrets.randbelow(50000),
                          password, str(image), espota.FLASH)
    if result == 0 and snapshot is not None:
        from history_transfer import restore_history
        restore_history("http://" + remote_ip, password, snapshot)
    return result


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as error:
        print("OTA upload failed: " + str(error), file=sys.stderr)
        sys.exit(1)
