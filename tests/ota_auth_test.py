"""Verify that the sensor challenges an OTA invitation and rejects a bad response."""
import socket
import sys

if len(sys.argv) != 2:
    raise SystemExit("Usage: python3 tests/ota_auth_test.py SENSOR_IP_OR_HOSTNAME")
target = socket.gethostbyname(sys.argv[1])
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as connection:
    connection.settimeout(8)
    connection.connect((target, 8266))
    # No firmware is sent; the deliberately incorrect digest must be rejected.
    connection.send(b"0 54321 392912 00000000000000000000000000000000\n")
    challenge = connection.recv(128).decode()
    assert challenge.startswith("AUTH "), "OTA did not require authentication"
    connection.send(b"200 11111111111111111111111111111111 00000000000000000000000000000000\n")
    result = connection.recv(128).decode()
    assert result == "Authentication Failed", "Bad authentication was not rejected: " + result
print("PASS: OTA requires authentication and rejects an incorrect password response.")
