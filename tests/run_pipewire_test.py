#!/usr/bin/env python3
"""Run audio integration against a private PipeWire server and capturing sinks."""
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time


CONFIG = """
context.properties = {
    core.daemon = true
    core.name = machoman-audio-test
    default.clock.rate = 48000
    default.clock.quantum = 256
    default.clock.min-quantum = 16
    default.clock.max-quantum = 2048
    support.dbus = false
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-access args = { access.legacy = true } }
]
context.objects = [
    { factory = spa-node-factory args = {
        factory.name = support.node.driver
        node.name = Dummy-Driver
        node.group = pipewire.dummy
        priority.driver = 20000
    } }
    { factory = metadata args = { metadata.name = default } }
]
"""


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def run():
    daemon_binary, sink_binary, machoman, venv, guest = sys.argv[1:]
    children = []
    with tempfile.TemporaryDirectory(prefix="machoman-pipewire-") as directory:
        root = Path(directory)
        (root / "pipewire.conf").write_text(CONFIG)
        env = os.environ.copy()
        env.update(PIPEWIRE_RUNTIME_DIR=directory, PIPEWIRE_REMOTE="machoman-audio-test", MACHOMAN_AUDIO_BACKEND="pipewire", MACHOMAN_AUDIO_TRACE="1")
        # Do not import desktop policy/config overrides into the isolated graph.
        for key in ("PIPEWIRE_CONFIG_DIR", "PIPEWIRE_CONFIG_NAME", "PIPEWIRE_CONFIG_PREFIX", "PIPEWIRE_PROPS", "PIPEWIRE_CORE"):
            env.pop(key, None)
        try:
            with (root / "daemon.log").open("w+") as daemon_log, (root / "sink.log").open("w+") as sink_log, (root / "guest.log").open("w+") as guest_log:
                daemon = subprocess.Popen([daemon_binary, "-c", str(root / "pipewire.conf")], env=env, stdout=daemon_log, stderr=subprocess.STDOUT)
                children.append(daemon)
                deadline = time.monotonic() + 5
                while not (root / "machoman-audio-test").exists():
                    if daemon.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("private PipeWire server did not start")
                    time.sleep(0.01)
                sink = subprocess.Popen([sink_binary], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sink_log, text=True, bufsize=1)
                children.append(sink)
                with selectors.DefaultSelector() as selector:
                    selector.register(sink.stdout, selectors.EVENT_READ)
                    if not selector.select(5) or sink.stdout.readline().strip() != "ready":
                        raise RuntimeError("PipeWire capture sinks did not become ready")
                process = subprocess.Popen([machoman, "run", venv, guest], env=env, stdout=subprocess.PIPE, stderr=guest_log, bufsize=0)
                children.append(process)
                deadline = time.monotonic() + 35
                transcript = []
                pending = b""
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)
                    while process.poll() is None:
                        if time.monotonic() >= deadline:
                            raise RuntimeError("guest audio test timed out")
                        if not selector.select(0.1):
                            continue
                        pending += os.read(process.stdout.fileno(), 4096)
                        while b"\n" in pending:
                            raw, pending = pending.split(b"\n", 1)
                            line = raw.decode("utf-8", errors="replace") + "\n"
                            transcript.append(line)
                            print(line, end="", flush=True)
                            if line.strip() == "CHANGE_DEFAULT":
                                sink.stdin.write("default\n"); sink.stdin.flush()
                            elif line.strip() == "REMOVE_DEVICE":
                                sink.stdin.write("remove\n"); sink.stdin.flush()
                            elif line.strip() == "DISCONNECT_DEVICE":
                                sink.stdin.write("disconnect\n"); sink.stdin.flush()
                tail = (pending + process.stdout.read()).decode("utf-8", errors="replace")
                print(tail, end="", flush=True)
                transcript.append(tail)
                if process.returncode or "CoreAudio PipeWire HAL" not in "".join(transcript):
                    raise RuntimeError(f"guest audio test exited with {process.returncode}")
                sink.stdin.write("quit\n"); sink.stdin.flush()
                output, _ = sink.communicate(timeout=5)
                print(output, end="", flush=True)
                if sink.returncode:
                    raise RuntimeError("PipeWire sample capture failed")
                print("PipeWire captured both PCM paths, channel layout and default-device routing")
        except Exception:
            for process in reversed(children):
                stop(process)
            for name in ("guest.log", "sink.log", "daemon.log"):
                path = root / name
                if path.exists():
                    print(f"--- {name} ---\n{path.read_text()[-12000:]}", file=sys.stderr)
            raise
        finally:
            for process in reversed(children):
                stop(process)


if __name__ == "__main__":
    run()
