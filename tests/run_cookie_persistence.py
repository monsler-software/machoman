"""Check cookie persistence across independent guest processes."""
from pathlib import Path
import stat
import subprocess
import sys
import tempfile

machoman, venv, binary = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="cookie-storage-") as directory:
    for phase in ("write", "read", "delete", "empty"):
        subprocess.run([machoman, "run", "-e", f"MACHOMAN_COOKIE_STORAGE_DIR={directory}", venv, binary, phase], check=True)
        files = list(Path(directory).glob("*.plist"))
        assert len(files) == 1, "one persistent jar"
        assert stat.S_IMODE(files[0].stat().st_mode) == 0o600, "cookie file must be private"
        assert stat.S_IMODE(Path(directory).stat().st_mode) == 0o700, "cookie directory must be private"
print("Persistent cookies, attributes, expiry, deletion and private storage passed")
