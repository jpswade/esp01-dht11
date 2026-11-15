#!/usr/bin/env python3
"""Create a device-specific OTA password once; preserve it on later builds."""
from pathlib import Path
import hashlib
import os
import secrets

project = Path(__file__).resolve().parent
private = project / "secrets"
private.mkdir(mode=0o700, exist_ok=True)
password_file = private / "ota-password.txt"
if not password_file.exists():
    fd = os.open(password_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as stream:
        stream.write(secrets.token_urlsafe(24) + "\n")
password = password_file.read_text().strip()
if len(password) < 20:
    raise SystemExit("OTA password is missing or too short; check " + str(password_file))
digest = hashlib.md5(password.encode()).hexdigest()
header = project / "HumiditySensor" / "ota_secrets.h"
header.write_text('#pragma once\n#define OTA_PASSWORD_HASH "' + digest + '"\n')
header.chmod(0o600)
print("OTA password saved at " + str(password_file) + "; existing password retained if present.")
