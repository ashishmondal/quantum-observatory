"""PlatformIO pre-build action: regenerate include/bitmaps/*.h from
assets/*.bmp before the C++ compiler runs.

Wired in via `extra_scripts = pre:tools/pre_build.py` in platformio.ini.
Failure of the converter aborts the build (Import + return code path).
"""

import sys, subprocess
from pathlib import Path

Import("env")  # noqa: F821 — provided by PlatformIO at script eval time

PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
SCRIPT      = PROJECT_DIR / "tools" / "bmp_to_header.py"

print("[pre_build] regenerating bitmap headers from assets/*.bmp")
result = subprocess.run([sys.executable, str(SCRIPT)],
                        cwd=str(PROJECT_DIR))
if result.returncode != 0:
    sys.exit(result.returncode)
