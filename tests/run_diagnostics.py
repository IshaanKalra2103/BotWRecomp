"""Exercise the real portable lifecycle, audio output and rotating logger without Apple SDKs."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
compilers = parser.add_mutually_exclusive_group()
compilers.add_argument('--cxx', default='clang++')
compilers.add_argument('--zig')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = root / 'build-tests'
build.mkdir(exist_ok=True)
exe = build / ('runtime_diagnostics.exe' if sys.platform == 'win32' else 'runtime_diagnostics')
command = [args.zig, 'c++'] if args.zig else [args.cxx]
command += ['-std=c++20', '-O1', '-Wall', '-Wextra', '-UNDEBUG', '-I', str(root / 'tests/audio_compat'),
            '-I', str(root), str(root / 'tests/runtime_diagnostics.cpp'), '-o', str(exe)]
if sys.platform != 'win32':
    command += ['-pthread']
if sys.platform == 'darwin':
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    command += ['-isysroot', sdk, '-stdlib=libc++']
subprocess.run(command, check=True)
with tempfile.TemporaryDirectory(prefix='diagnostics-', dir=build) as directory:
    with (build / 'diagnostics-test-stderr.txt').open('wb') as stderr:
        subprocess.run([str(exe), directory], stderr=stderr, check=True, timeout=60)
