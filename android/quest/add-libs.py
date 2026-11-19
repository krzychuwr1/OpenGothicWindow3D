"""Adds native libraries to an APK produced by aapt2 (used by build-quest.ps1)."""
import sys
import zipfile

apk = sys.argv[1]
with zipfile.ZipFile(apk, "a", compression=zipfile.ZIP_DEFLATED) as z:
    for path in sys.argv[2:]:
        name = path.replace("\\", "/").rsplit("/", 1)[-1]
        z.write(path, "lib/arm64-v8a/" + name)
