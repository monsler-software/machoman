#!/usr/bin/env python3
"""Run the installed Live Trial and capture guest stacks when startup stalls."""
import argparse
import ctypes
import os
from pathlib import Path
import resource
import signal
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--seconds", type=float, default=240)
parser.add_argument("--keep-running", action="store_true", help="leave Live running without a time deadline; retain the RSS guard")
parser.add_argument("--build-dir", type=Path, help="use an isolated machoman build and its host frameworks")
parser.add_argument("--quiet-appkit", action="store_true", help="disable per-event/per-frame AppKit tracing for latency measurements")
parser.add_argument("--frame-timing", action="store_true", help="report aggregate update/draw/present times every three seconds")
parser.add_argument("--cache-trace", action="store_true", help="report disk translation cache hits and writes")
parser.add_argument("--log", type=Path, default=Path("work/live-diagnostic.log"))
parser.add_argument("--objc", action="store_true", help="trace Objective-C messages")
parser.add_argument("--force-show", action="store_true", help="show windows for drawing diagnostics")
parser.add_argument("--mevk", action="store_true", help="prefer the built Metal/MEVK framework and trace GPU submissions")
parser.add_argument("--mevk-present", choices=("auto", "vulkan", "cpu"), help="select MEVK drawable output; vulkan requires direct Wayland presentation")
parser.add_argument("--strict-selectors", action="store_true", help="stop at the first missing Objective-C method instead of returning nil")
parser.add_argument("--no-guest-subprocesses", action="store_true", help="isolate UI diagnostics from memory-heavy Mach-O helper processes")
parser.add_argument("--gdb", action="store_true", help="attach GDB and save native thread stacks at the first fault without a core dump")
parser.add_argument("--watch", help="comma-separated guest instruction addresses to trace")
parser.add_argument("--watch-limit", type=int, default=32, help="maximum log lines per watch address; 0 means unlimited")
parser.add_argument("--watch-dump", help="VA:register:byte-count[:pointer-offset[:data-offset]] memory snapshots at watched instructions")
parser.add_argument("--backtrace", help="comma-separated Objective-C selectors to trace with stacks")
parser.add_argument("--invalid-objc-backtrace", action="store_true", help="print stacks for messages to invalid Objective-C objects")
parser.add_argument("--dump-after", type=float, help="capture the interrupted PC and guest stacks after this many seconds")
parser.add_argument("--dump-every", type=float, default=30, help="interval between stack captures with --dump-after")
parser.add_argument("--rss-limit-gb", type=float, default=5, help="stop if Live RSS exceeds this many GiB; use a cgroup for a hard limit")
parser.add_argument("--inspect-pid", type=int, help="sample an existing Live process without launching or stopping it")
parser.add_argument("--stop-pid", type=int, help="terminate an existing machoman Live process after validating its command line")
parser.add_argument("--inspect-samples", type=int, default=1, help="number of SIGQUIT stack samples of --inspect-pid")
args = parser.parse_args()
if args.seconds <= 0:
    parser.error("--seconds must be positive")
if args.dump_after is not None and (args.dump_after <= 0 or args.dump_every <= 0):
    parser.error("stack capture times must be positive")
if args.watch_limit < 0:
    parser.error("--watch-limit must be nonnegative")
if args.rss_limit_gb <= 0:
    parser.error("--rss-limit-gb must be positive")
root = Path(__file__).resolve().parent.parent
build_dir = args.build_dir.resolve() if args.build_dir else root / "build"
log_path = args.log if args.log.is_absolute() else root / args.log
log_path.parent.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env.update(MACHOMAN_OSLOG="1", MACHOMAN_APPKIT_TRACE="1", DYLD_PRINT_LIBRARIES="1")
if args.quiet_appkit:
    env.pop("MACHOMAN_APPKIT_TRACE", None)
if args.frame_timing:
    env["MACHOMAN_APPKIT_FRAME_TIMING"] = "1"
if args.cache_trace:
    env["MACHOMAN_CACHE_TRACE"] = "1"
if args.mevk:
    env["DYLD_FRAMEWORK_PATH"] = str(build_dir / "dylibs/System/Library/Frameworks")
    if args.quiet_appkit:
        env.pop("MACHOMAN_MEVK_TRACE", None)
    else:
        env["MACHOMAN_MEVK_TRACE"] = "1"
if args.mevk_present:
    env["MACHOMAN_MEVK_PRESENT"] = args.mevk_present
if args.strict_selectors:
    env["MACHOMAN_STRICT_SELECTORS"] = "1"
if args.objc:
    env["MACHOMAN_OBJC_TRACE"] = "1"
if args.force_show:
    env["MACHOMAN_APPKIT_FORCE_SHOW"] = "1"
if args.watch:
    env["MACHOMAN_WATCH"] = args.watch
    env["MACHOMAN_WATCH_LIMIT"] = str(args.watch_limit)
if args.watch_dump:
    dump_dir = log_path.with_suffix(".dumps")
    dump_dir.mkdir(parents=True, exist_ok=True)
    env["MACHOMAN_WATCH_DUMP"] = args.watch_dump
    env["MACHOMAN_WATCH_DUMP_DIR"] = str(dump_dir)
if args.backtrace:
    env["MACHOMAN_OBJC_BACKTRACE"] = args.backtrace
if args.invalid_objc_backtrace:
    env["MACHOMAN_OBJC_INVALID_BACKTRACE"] = "1"
env["MACHOMAN_DIAGNOSTICS"] = "1"
if args.no_guest_subprocesses:
    env["MACHOMAN_DISABLE_GUEST_SUBPROCESSES"] = "1"

def disable_core_dump():
    # Multi-GiB diagnostic cores must not consume RAM-backed /tmp or page cache.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    if args.gdb:
        # GDB is a sibling of Live under this launcher. Allow it to attach
        # with Yama ptrace_scope=1, for this diagnostic child only.
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(ctypes.c_int(0x59616D61), ctypes.c_ulong(-1),
                      ctypes.c_ulong(0), ctypes.c_ulong(0), ctypes.c_ulong(0)) != 0:
            raise OSError(ctypes.get_errno(), "PR_SET_PTRACER for diagnostic GDB")

def memory_usage(pid):
    try:
        fields = {}
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith(("VmRSS:", "VmSize:", "VmSwap:")):
                name, value, _ = line.split()
                fields[name.rstrip(":")] = int(value) * 1024
        return fields
    except (FileNotFoundError, ProcessLookupError):
        return {}

if args.inspect_pid is not None or args.stop_pid is not None:
    if args.inspect_pid is not None and args.stop_pid is not None:
        parser.error("choose --inspect-pid or --stop-pid")
    target_pid = args.stop_pid if args.stop_pid is not None else args.inspect_pid
    if args.inspect_samples < 1 or args.inspect_samples > 30:
        parser.error("--inspect-samples must be between 1 and 30")
    try:
        command = Path(f"/proc/{target_pid}/cmdline").read_bytes().split(b"\0")
    except FileNotFoundError:
        parser.exit(1, f"Live PID {target_pid} no longer exists\n")
    if command == [b""]:
        parser.exit(1, f"Live PID {target_pid} has exited (waiting for its launcher to reap it)\n")
    if not command or Path(os.fsdecode(command[0])).name != "machoman" or b"Live" not in command:
        parser.error("PID must refer to the running machoman Live process")
    if args.stop_pid is not None:
        os.kill(target_pid, signal.SIGTERM)
        print(f"Sent SIGTERM to Live PID {target_pid}", flush=True)
        raise SystemExit(0)
    for sample in range(args.inspect_samples):
        usage = memory_usage(args.inspect_pid)
        print(f"Snapshot {sample + 1}: Live PID {args.inspect_pid}; RSS {usage.get('VmRSS', 0) / 2**30:.2f} GiB", flush=True)
        for name in ["wchan", "io"]:
            print(f"{name}: {Path(f'/proc/{args.inspect_pid}/{name}').read_text().strip()}", flush=True)
        proc_root = Path(f"/proc/{args.inspect_pid}")
        if sample == 0:
            print(f"mounts:\n{(proc_root / 'mountinfo').read_text()}", flush=True)
        for fd_path in sorted((proc_root / "fd").iterdir(), key=lambda p: int(p.name)):
            try:
                target = os.readlink(fd_path)
                if target.startswith("/") and target not in ("/dev/null", "/dev/urandom", "/dev/random"):
                    info = (proc_root / "fdinfo" / fd_path.name).read_text().strip()
                    print(f"fd {fd_path.name}: {target}\n{info}", flush=True)
            except (FileNotFoundError, ProcessLookupError):
                pass
        os.kill(args.inspect_pid, signal.SIGQUIT)
        time.sleep(1)
    raise SystemExit(0)

if args.mevk:
    for framework in ("Metal", "CoreGraphics"):
        binary = build_dir / f"dylibs/System/Library/Frameworks/{framework}.framework/Versions/A/{framework}.so"
        if not binary.is_file() or binary.resolve().name == "machoman-stub-framework.so":
            parser.error(f"--mevk requires a built MEVK {framework} framework in {build_dir}; "
                         f"configure with -DMACHOMAN_BUILD_MEVK=ON and run cmake --build")

with log_path.open("wb") as log, log_path.with_suffix(".memory.csv").open("w", buffering=1) as memory_log:
    memory_log.write("seconds,rss_bytes,virtual_bytes,swap_bytes\n")
    process = subprocess.Popen([str(build_dir / "machoman"), "run", str(root / "work/venv"), "Live"],
                               cwd=root, env=env, stdout=log, stderr=log, start_new_session=True,
                               preexec_fn=disable_core_dump)
    print(f"Live PID {process.pid}; log: {log_path}", flush=True)
    debugger = None
    debugger_log = None
    if args.gdb:
        debugger_path = log_path.with_suffix(".host-stack.log")
        debugger_log = debugger_path.open("wb")
        debugger = subprocess.Popen([
            "gdb", "-q", "-batch", "-iex", "set debuginfod enabled off",
            "-ex", "set pagination off", "-ex", "handle SIGQUIT nostop noprint pass",
            "-ex", "handle SIGPIPE nostop noprint pass", "-ex", "handle SIGUSR1 nostop noprint pass",
            "-p", str(process.pid), "-ex", "continue",
            "-ex", "thread apply all bt 16", "-ex", "info registers",
            "-ex", "p '(anonymous namespace)::guest_process::g_current'",
            "-ex", "p/x '(anonymous namespace)::guest_process::g_current'->ctx", "-ex", "detach",
        ], stdout=debugger_log, stderr=debugger_log)
        print(f"GDB fault capture: {debugger_path}", flush=True)
    start = time.monotonic()
    dumped = False
    next_dump = args.dump_after
    next_memory_report = 0
    try:
        while process.poll() is None and (args.keep_running or time.monotonic() - start < args.seconds):
            time.sleep(1)
            elapsed = time.monotonic() - start
            usage = memory_usage(process.pid)
            if usage:
                rss = usage.get("VmRSS", 0)
                memory_log.write(f"{elapsed:.1f},{rss},{usage.get('VmSize', 0)},{usage.get('VmSwap', 0)}\n")
                if elapsed >= next_memory_report:
                    print(f"Live memory at {elapsed:.0f}s: RSS {rss / 2**30:.2f} GiB; virtual {usage.get('VmSize', 0) / 2**30:.2f} GiB", flush=True)
                    next_memory_report = elapsed + 30
                if rss > args.rss_limit_gb * 2**30 and process.poll() is None:
                    print(f"Stopping Live: RSS exceeded {args.rss_limit_gb:g} GiB; samples saved beside the log", flush=True)
                    process.send_signal(signal.SIGQUIT)
                    dumped = True
                    break
            if next_dump is not None and time.monotonic() - start >= next_dump and process.poll() is None:
                process.send_signal(signal.SIGQUIT)
                dumped = True
                next_dump += args.dump_every
                print(f"Captured guest stacks at {time.monotonic() - start:.0f}s.", flush=True)
            if not dumped and time.monotonic() - start > 120:
                with log_path.open("rb") as reader:
                    reader.seek(max(0, log_path.stat().st_size - 8192))
                    idle = b"event loop idle" in reader.read()
                if idle and process.poll() is None:
                    process.send_signal(signal.SIGQUIT)
                    dumped = True
                    print("Captured guest thread stacks after startup became idle.", flush=True)
    finally:
        if process.poll() is None:
            with log_path.open("rb") as reader:
                app_loop_started = b"AppKit: -[NSApplication run]" in reader.read(1024 * 1024)
            if not dumped and app_loop_started:
                process.send_signal(signal.SIGQUIT)
                time.sleep(1)
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        else:
            # Helpers must not outlive a crashed or externally stopped UI.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        if debugger is not None:
            try:
                debugger.wait(timeout=3)
            except subprocess.TimeoutExpired:
                debugger.terminate()
                try:
                    debugger.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    debugger.kill()
                    debugger.wait()
            debugger_log.close()
    print(f"Live exited with status {process.returncode} after {time.monotonic() - start:.1f}s", flush=True)
