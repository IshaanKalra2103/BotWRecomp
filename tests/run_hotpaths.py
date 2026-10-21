"""Compile and run host-only regression tests, without game files or the Apple SDK.

python tests/run_hotpaths.py --cxx clang++
python tests/run_hotpaths.py --zig /path/to/zig

SDL and POSIX file APIs are deterministic test doubles, not platform integration tests.
"""
import argparse
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
compiler = parser.add_mutually_exclusive_group()
compiler.add_argument("--cxx", default="clang++")
compiler.add_argument("--zig")
parser.add_argument("--compare-upstream", action="store_true", help="also benchmark the Git HEAD menu")
args = parser.parse_args()
build = root / "build-tests"
build.mkdir(exist_ok=True)
command = [args.zig, "c++"] if args.zig else [args.cxx]
common = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Wno-unused-variable", "-UNDEBUG",
          "-I", str(root / "tests/compat"), "-I", str(root), "-I", str(root / "runtime/src")]
if args.zig:
    common += ["-Wno-nullability-completeness"]
if sys.platform != "win32":
    common += ["-pthread"]
if sys.platform == "darwin":
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    common += ['-isysroot', sdk, '-stdlib=libc++']
for name in ("ui_hotpaths", "fs_case_cache"):
    exe = build / (name + (".exe" if sys.platform == "win32" else ""))
    subprocess.run(command + common + [str(root / "tests" / f"{name}.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

# Compile both real overlay callers against the vendored ImGui headers. Cemu normally provides
# sint32 from its precompiled header; the standalone check only needs that integer alias.
for name in ("overlay_menu", "overlay_touch"):
    subprocess.run(command + common + ["-I", str(root / "runtime/cemu"),
                   "-I", str(root / "runtime/cemu/dependencies/imgui"), "-Dsint32=int",
                   "-Wno-nontrivial-memcall", "-c", str(root / "runtime/cemu_shims" / f"{name}.cpp"),
                   "-o", str(build / f"{name}.o")], check=True)
print("Both overlay translation units compiled against the vendored ImGui headers.", flush=True)
if not args.compare_upstream:
    sys.exit(0)

# Compare allocations against the unmodified upstream menu using exactly the same harness.
result = subprocess.run(["git", "show", "HEAD:runtime/src/host/menu.cpp"], cwd=root,
                        capture_output=True, text=True, check=True)
baseline = build / "baseline_menu.cpp"
baseline.write_text(result.stdout, encoding="utf-8")
exe = build / ("menu_baseline.exe" if sys.platform == "win32" else "menu_baseline")
subprocess.run(command + common + ["-I", str(root / "runtime/src/host"),
               "-DBOTW_MENU_BENCH_ONLY", '-DBOTW_MENU_SOURCE="build-tests/baseline_menu.cpp"',
               str(root / "tests/ui_hotpaths.cpp"), "-o", str(exe)], check=True)
print("Upstream comparison:", flush=True)
subprocess.run([str(exe)], check=True)
