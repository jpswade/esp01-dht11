import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch


class UploadPreflightTests(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[1] / "upload_ota.py"
        spec = importlib.util.spec_from_file_location("upload_under_test", path)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        self.module.__file__ = str(self.project / "upload_ota.py")
        (self.project / "secrets").mkdir()
        (self.project / "build-local").mkdir()
        self.password = "test-only-password-not-a-device-secret"
        (self.project / "secrets/ota-password.txt").write_text(self.password)
        self.image = self.project / "build-local/HumiditySensor.ino.bin"
        self.image.write_bytes(b"test image " + hashlib.md5(self.password.encode()).hexdigest().encode())
        self.uploader_path = self.project / "espota.py"
        self.uploader_path.touch()

    def upload(self, firmware="history-uptime-v4", hostname="humidity-test", expected=None):
        device = dict(firmware=firmware, hostname=hostname, ota_enabled=True, free_sketch_space=1000000)
        opener = Mock()
        opener.open.return_value = io.BytesIO(json.dumps(device).encode())
        uploader = Mock(FLASH=0)
        uploader.serve.return_value = 0
        argv = ["upload_ota.py", "--host", "192.0.2.1", "--espota", str(self.uploader_path)]
        if expected:
            argv += ["--expected-hostname", expected]
        spec = Mock()
        with patch("sys.argv", argv), \
             patch.object(self.module.socket, "gethostbyname", return_value="192.0.2.1"), \
             patch.object(self.module.urllib.request, "build_opener", return_value=opener), \
             patch.object(self.module.importlib.util, "spec_from_file_location", return_value=spec), \
             patch.object(self.module.importlib.util, "module_from_spec", return_value=uploader), \
             patch.object(self.module.socket, "socket") as route:
            route.return_value.__enter__.return_value.getsockname.return_value = ("192.0.2.2", 0)
            result = self.module.main()
        return result, uploader

    def test_current_uptime_firmware_is_accepted(self):
        result, uploader = self.upload(expected="humidity-test")
        self.assertEqual(result, 0)
        self.assertEqual(uploader.serve.call_args.args[0], "192.0.2.1")
        self.assertEqual(uploader.serve.call_args.args[4], self.password)

    def test_original_ota_firmware_is_accepted(self):
        result, uploader = self.upload(firmware="history-ota-v1")
        self.assertEqual(result, 0)
        uploader.serve.assert_called_once()

    def test_wrong_device_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.upload(expected="humidity-another-device")

    def test_unrelated_firmware_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.upload(firmware="unrelated-firmware")

    def test_password_and_image_must_match(self):
        self.image.write_bytes(b"image made with a different password")
        with self.assertRaises(SystemExit):
            self.upload()


if __name__ == "__main__":
    unittest.main()
