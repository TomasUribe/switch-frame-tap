#!/usr/bin/env python3
"""Package the preview APK and its complete corresponding Android source."""
import hashlib
from pathlib import Path
import shutil
import zipfile

project = Path(__file__).resolve().parents[1]
dist = project.parent / "dist"
dist.mkdir(exist_ok=True)
version = "0.1.2-preview"
apk = dist / f"SwitchFrameTapTV-{version}.apk"
shutil.copy2(project / "app/build/outputs/apk/debug/app-debug.apk", apk)
archive = dist / f"SwitchFrameTapTV-{version}-source.zip"
excluded = {".gradle", ".gradle-user-home", "build", "test-output", "__pycache__"}
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
    for path in sorted(project.rglob("*")):
        relative = path.relative_to(project)
        if path.is_file() and not any(part in excluded for part in relative.parts) and path.name != "local.properties":
            out.write(path, Path("android-tv") / relative)
checksums = dist / f"SwitchFrameTapTV-{version}-SHA256.txt"
checksums.write_text("".join(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
                             for path in (apk, archive)), encoding="ascii")
for path in (apk, archive, checksums):
    print(f"{path.name}: {path.stat().st_size:,} bytes")
