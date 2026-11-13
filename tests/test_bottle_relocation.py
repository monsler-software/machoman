"""Exercise relocation failure atomicity, signed Mach-O and moved venvs."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

fixture, machoman, sdk, clang, tool, relocator = sys.argv[1:]
fixture = Path(fixture)
fixture.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location("relocator", relocator)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
environment = dict(os.environ, MACHOMAN_TRANSLATION_CACHE="0")
environment.pop("DYLD_INSERT_LIBRARIES", None)

def run(arguments, *, success=True):
    result = subprocess.run(arguments, env=environment, capture_output=True, text=True, timeout=30)
    assert (result.returncode == 0) == success, (arguments, result.stdout, result.stderr)
    return result

with tempfile.TemporaryDirectory(prefix="case-", dir=fixture) as temporary:
    root = Path(temporary)
    venv = root / "venv with spaces"
    run([machoman, "venv-init", str(venv)])
    keg = venv / "opt/homebrew/Cellar/demo/1"
    (keg / "lib").mkdir(parents=True)
    (keg / "bin").mkdir()
    library = keg / "lib/libdemo.dylib"
    binary = keg / "bin/demo"
    lib_source = root / "lib.c"
    main_source = root / "main.c"
    lib_source.write_text('const char *bottle_value(void) { return "@@HOMEBREW_PREFIX@@/share/demo"; }\n')
    main_source.write_text('#include <stdio.h>\nextern const char *bottle_value(void);\nint main(void) { puts(bottle_value()); puts("@@HOMEBREW_PREFIX@@/Cellar/demo/1"); return 0; }\n')
    (keg / "paths.pc").write_text("cellar=@@HOMEBREW_CELLAR@@\nrepository=@@HOMEBREW_REPOSITORY@@\n")
    common = [clang, "--target=arm64-apple-macos12", "-isysroot", sdk, "-fuse-ld=lld"]
    run(common + ["-dynamiclib", "-Wl,-install_name,@@HOMEBREW_PREFIX@@/opt/demo/lib/libdemo.dylib", str(lib_source), "-o", str(library)])
    run(common + [str(main_source), str(library), "-Wl,-rpath,@@HOMEBREW_PREFIX@@/lib", "-o", str(binary)])
    (venv / "opt/homebrew/opt").mkdir()
    (venv / "opt/homebrew/opt/demo").symlink_to("../Cellar/demo/1")
    (venv / "bin/demo").symlink_to("../opt/homebrew/Cellar/demo/1/bin/demo")
    original = {path: path.read_bytes() for path in (binary, library)}
    malformed = keg / "unsupported.bin"
    malformed.write_bytes(b"\0@@HOMEBREW_PREFIX@@")
    command = [sys.executable, "-B", relocator, "--venv", str(venv), "--install-name-tool", tool]
    run(command, success=False)
    assert all(path.read_bytes() == value for path, value in original.items()), "preflight failure changed original files"
    malformed.unlink()
    run(command)
    for path in (binary, library):
        data = path.read_bytes()
        assert not module.contains_tokens(data)
        assert module.verify_code_signature(data)
    _, sections = module.macho_layout(original[binary])
    try:
        module.patch_cstrings(original[binary], sections, {module.TOKENS[0]: b"x" * 200})
    except ValueError:
        pass
    else:
        raise AssertionError("oversized binary path was accepted")
    result = run([machoman, "run", str(venv), "demo"])
    assert "/opt/homebrew/share/demo" in result.stdout and "/opt/homebrew/Cellar/demo/1" in result.stdout, result
    moved = root / "copied venv"
    shutil.copytree(venv, moved, symlinks=True)
    result = run([machoman, "run", str(moved), "demo"])
    assert "/opt/homebrew/share/demo" in result.stdout, result
    run(command + ["--check"])
    before = binary.read_bytes()
    run(command)
    assert binary.read_bytes() == before, "relocation is not idempotent"
    assert "cellar=/opt/homebrew/Cellar" in (keg / "paths.pc").read_text()
print("Bottle relocation atomicity, signed paths, embedded strings and copied venv passed")
