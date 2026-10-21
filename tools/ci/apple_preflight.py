"""Syntax-check changed Apple translation units before compiling 1,125 guest units."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess

SOURCES = {
    'ios/src/main_ios.mm', 'ios/src/diagnostics_ios.mm',
    'runtime/src/main.cpp', 'runtime/src/log.cpp', 'runtime/src/audio/output.cpp',
    'runtime/src/host/lifecycle.cpp', 'runtime/src/host/window.mm', 'runtime/src/host/touch.cpp',
    'runtime/src/snd/ax_ist.cpp', 'runtime/cemu_shims/overlay_menu.cpp',
    'runtime/cemu/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.cpp',
    'runtime/cemu/Cafe/HW/Latte/Renderer/Metal/MetalLayerHandle.cpp',
    'runtime/cemu/Cafe/HW/Latte/Core/LatteShaderCache.cpp',
}

def syntax_command(command):
    result = []
    skip_next = False
    for item in command:
        if skip_next:
            skip_next = False
            continue
        if item in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
            skip_next = True
        elif item not in ('-c', '-MD', '-MMD', '-MP'):
            result.append(item)
    return result + ['-fsyntax-only']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('database', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    entries = json.loads(args.database.read_text())
    checked = set()
    failures = []
    for entry in entries:
        source = Path(entry['file'])
        if not source.is_absolute(): source = Path(entry['directory']) / source
        try: name = source.resolve().relative_to(root).as_posix()
        except ValueError: continue
        if name not in SOURCES or name in checked: continue
        checked.add(name)
        print('Apple preflight: ' + name, flush=True)
        command = entry.get('arguments') or shlex.split(entry['command'])
        result = subprocess.run(syntax_command(command), cwd=entry['directory'])
        if result.returncode: failures.append(name)
    missing = SOURCES - checked
    if failures or missing:
        raise SystemExit(f'Apple preflight failed: {failures}; missing compilation entries: {sorted(missing)}')
    print(f'Apple preflight: all {len(checked)} real device translation units passed.', flush=True)

if __name__ == '__main__': main()
