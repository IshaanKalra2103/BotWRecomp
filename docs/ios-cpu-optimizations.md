# iOS CPU overhead reductions

Based on upstream commit `2b84b82dbe7efe41707d236fdbac9ec5e98c8d3d`.
These changes reduce host CPU work. The operation counts below are isolated host microbenchmarks,
not measurements of an iPhone FPS improvement. The integrated runtime has since been tested on an
iPhone 15 Pro alongside the lifecycle/audio changes described in [iOS diagnostics](ios-diagnostics.md).
No game files, generated game code or shader caches are included.

## Changes

- Closed menus return an empty snapshot before reading settings or formatting rows. The overlay
  also skips requesting a menu snapshot while it is closed.
- Touch controls fill a caller-owned snapshot whose vector storage survives between frames.
  The FPS overlay and touch renderer share that snapshot instead of independently copying it.
  The snapshot is still made under the existing input mutex; pressed/hidden/visible states reset
  on every update.
- iOS game-path resolution caches successful exact and case-corrected paths per thread. Warm cache
  hits require neither `access()` probes nor the shared directory-cache mutex. Each thread stores
  at most 256 paths; filling the cache clears it before insertion. Missing paths are not cached.
  Only the read-only game folder is cached: editing or replacing game files requires restarting
  the app. Saves outside the game folder bypass the cache. Prefix checks respect directory
  boundaries (`/game-other` is not inside `/game`).
- The GPU thread handle uses value initialization, which is also valid for the integer thread
  handles used by the standalone Windows compilation checks.

## Host validation

Run with Python 3.12+ and a C++20 compiler:

```sh
python tests/run_hotpaths.py --cxx clang++
# Or a Zig distribution with its bundled C++ compiler:
python tests/run_hotpaths.py --zig /path/to/zig
# Optional comparison requires an unmodified upstream Git HEAD:
python tests/run_hotpaths.py --zig /path/to/zig --compare-upstream
```

The tests compile the real menu, touch and path-resolution implementations using deterministic
SDL/POSIX test doubles. They check input press/release, D-pad reset, stick/camera reset, collapsed
and hidden controls, snapshot storage reuse, correct case resolution, missing-file retries,
directory boundaries, cache bounds and per-thread cache isolation. Both real overlay translation
units also compile against the vendored ImGui headers. These checks do not validate UIKit, Metal,
code signing, SDK compatibility or a full Apple build.

Measured on Windows with Zig 0.16.0, optimized C++20 builds and assertions enabled:

| Workload | Upstream | Changed |
| --- | ---: | ---: |
| 10,000 closed-menu snapshots: heap allocations | 30,000 | 0 |
| 10,000 closed-menu snapshots: settings reads | 10,000 | 0 |
| 10,000 closed-menu snapshots: constructed rows | 110,000 | 0 |
| 10,000 warmed touch snapshots: heap allocations | not measured | 0 |
| 20,000 warmed exact/mixed-case path lookups: access calls | not measured | 0 |
| Same warmed path lookups: directory reads | not measured | 0 |

These are operation counts in isolated tests, not FPS measurements or total application allocation
counts. The cache trades a bounded amount of per-thread memory for fewer file-system calls.

## Controlled device comparison still needed

Build both upstream and the modified version on an Apple Silicon Mac with the same game RPX and
Xcode version, following `README.md`. Use the same save and route, with identical settings,
battery/thermal conditions and shader-cache state. A reasonable initial comparison is 30 fps,
1x resolution and bilinear filtering, followed by 60 fps with otherwise identical settings.
Let first-use shaders settle before recording sustained behavior. Do not compare a cold shader
cache against a warmed cache.

Compare game frame times as well as the GPU FPS counter, CPU/GPU utilization, process footprint and
thermal state. Check touch controls with the FPS counter both on and off, open/close the menu, and
exercise repeated asset loads and saves. Run for at least 20 minutes. Log any recurring background
stall or gameplay hang separately; successful short focus-recovery tests do not establish
long-session stability.

If the device is GPU-limited, an additional test at 0.75x (960x540) can assess the image-quality
tradeoff. It renders 43.75% fewer internal pixels than 1x (1280x720), which does not imply the same
percentage change in frame time. The CPU hot-path changes alone do not change performance defaults;
the integrated iPhone 15 Pro starting profile is documented in [iOS diagnostics](ios-diagnostics.md).
