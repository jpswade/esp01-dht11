#!/usr/bin/env python3
"""Install checksum-pinned Arduino libraries into the ignored vendor directory."""
from pathlib import Path, PurePosixPath
from urllib.request import urlopen
import hashlib
import io
import json
import tempfile
import zipfile


def main():
    project = Path(__file__).resolve().parent
    vendor = project / "vendor"
    vendor.mkdir(exist_ok=True)
    for library in json.loads((project / "dependencies.lock.json").read_text())["libraries"]:
        name = library["directory"]
        target = vendor / name
        if target.exists():
            raise SystemExit(f"{target} already exists; move it aside before a clean install.")
        with urlopen(library["url"], timeout=30) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != library["sha256"]:
            raise SystemExit(f"Checksum mismatch for {name}; nothing extracted.")
        with tempfile.TemporaryDirectory(dir=vendor) as temporary:
            root = Path(temporary)
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                for member in archive.infolist():
                    path = PurePosixPath(member.filename)
                    if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != name:
                        raise SystemExit(f"Unexpected archive path for {name}.")
                archive.extractall(root)
            (root / name).rename(target)
        print("Installed " + name)


if __name__ == "__main__":
    main()
