"""Local history backup and bounded digest-authenticated restoration after OTA."""
from pathlib import Path
from datetime import datetime, timezone
import json
import math
import struct
import time
import urllib.request

WINDOW_MS = 86400000
CAPACITY = 289


def get_json(base, path):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(base + path, timeout=15) as response:
        return json.load(response)


def save_history(base, folder):
    history = get_json(base, "/api/history")
    captured = time.time()
    if not isinstance(history.get("readings"), list) or len(history["readings"]) > CAPACITY:
        raise ValueError("Unexpected history response; refusing update")
    snapshot = {"captured_unix_s": captured, "history": history}
    folder.mkdir(exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d-%H%M%S-%f")
    path = folder / ("before-ota-" + stamp + ".json")
    with path.open("x") as output:
        json.dump(snapshot, output, indent=2, allow_nan=False)
        output.write("\n")
    if json.loads(path.read_text()) != snapshot:
        raise ValueError("History backup read-back failed; refusing update")
    print(f"History saved: {path} ({len(history['readings'])} points)", flush=True)
    return snapshot


def encode_history(snapshots, now):
    points = []
    for snapshot in snapshots:
        elapsed_ms = max(0, int((now - snapshot["captured_unix_s"]) * 1000))
        for point in snapshot["history"]["readings"]:
            age = int(point["age_ms"]) + elapsed_ms
            if age < 0 or age > WINDOW_MS:
                continue
            values = []
            for key, low, high in (("humidity_percent", 0, 100), ("temperature_c", -50, 100)):
                value = point[key]
                if value is not None and (not math.isfinite(value) or not low <= value <= high):
                    raise ValueError("Invalid history value")
                values.append(float("nan") if value is None else value)
            points.append((age, *values))
    points.sort(reverse=True, key=lambda point: point[0])
    unique = []
    for point in points:
        if unique and point[0] == unique[-1][0]:
            unique[-1] = point
        else:
            unique.append(point)
    points = unique[-CAPACITY:]
    return b"HST1" + struct.pack("<I", len(points)) + b"".join(struct.pack("<Iff", *point) for point in points)


def restore_history(base, password, snapshot):
    device = None
    for attempt in range(20):
        try:
            device = get_json(base, "/api/device")
            break
        except (OSError, ValueError):
            time.sleep(2)
    if not device or not device.get("history_restore"):
        raise ValueError("Updated sensor does not provide history restoration; the saved file is retained")
    live = get_json(base, "/api/history")
    current = {"captured_unix_s": time.time(), "history": live}
    payload = encode_history([snapshot, current], time.time())
    count = struct.unpack_from("<I", payload, 4)[0]
    boundary = "humidity-history-transfer-boundary"
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="history"; filename="history.bin"\r\n'
            'Content-Type: application/octet-stream\r\n\r\n').encode() + payload + f"\r\n--{boundary}--\r\n".encode()
    passwords = urllib.request.HTTPPasswordMgrWithDefaultRealm()
    endpoint = base + "/api/history/restore"
    passwords.add_password("humidity-history", endpoint, "history", password)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), urllib.request.HTTPDigestAuthHandler(passwords))
    request = urllib.request.Request(endpoint, data=body,
        headers={"Content-Type": "multipart/form-data; boundary=" + boundary}, method="POST")
    with opener.open(request, timeout=30) as response:
        result = json.load(response)
    if result.get("restored") != count:
        raise ValueError("Sensor did not confirm the complete history restore")
    actual = get_json(base, "/api/history")["readings"]
    if len(actual) != count:
        raise ValueError("History count failed read-back after restore")
    for index, point in enumerate(actual):
        expected = struct.unpack_from("<Iff", payload, 8 + index * 12)
        for key, value in zip(("humidity_percent", "temperature_c"), expected[1:]):
            if (math.isnan(value) and point[key] is not None) or (not math.isnan(value) and point[key] != round(value, 1)):
                raise ValueError("History values failed read-back after restore")
    print(f"History restored and verified: {count} points (including new startup readings)", flush=True)
    return count
