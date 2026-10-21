# iOS focus recovery, audio and diagnostics

The iOS runtime stops submitting Metal command buffers when its app or scene loses activity,
including Control Center. A submission mutex closes the race between the GPU thread and UIKit;
already committed work is drained before the lifecycle callback returns. The GPU thread waits
for activation before submitting more work or acquiring another drawable.

On iOS, command buffers no longer form a chain of Metal event waits/signals. A failed buffer
cannot leave later buffers waiting for a signal it never sends. Commands still use one queue
and the renderer's existing resource tracking and barriers. At most 64 submitted buffers may
remain pending before allocation waits for the oldest one. macOS retains its existing event path.
Command-buffer failures are logged with their code, domain and description.

## Audio

The AX host thread sets interactive QoS and pauses its frame generator while inactive. SDL output
retains the 48 kHz stereo format and user volume, but starts after collecting 60 ms of samples.
Scene deactivation, audio-session interruptions and output-route changes clear stale samples and
require priming again. Queues over 250 ms and producer gaps over 80 ms also reset output.
An output-route change does not clear an ongoing interruption.

Every two seconds the log records audio level, queued duration, clipping, producer gaps, AX mixing
time and scheduler lateness. `shortage_requests` counts SDL requests for additional samples;
it is not a count of proven audible glitches. The real AX 32-to-48 kHz resampler is regression
tested for pitch, duration, channels, endian conversion and sample history; its coefficients
are unchanged.

## Persistent logs

The usual `Library/Application Support/BotWRecomp/runtime.log` is mirrored to
`Documents/Diagnostics/runtime.log`, accessible in Files or through app file sharing.
Both locations rotate at launch and at 16 MiB, retaining four previous files each.
Logs stay on the device; no upload service is configured.

Records include the build revision, UTC session start, microsecond monotonic timestamps, sequence
number and thread ID. Periodic records cover process footprint, available memory before jetsam,
thermal state, low-power mode, GPU queue state and audio timing. Lifecycle transitions, memory
warnings and audio-session notifications are recorded separately. Exception reporting and a
minimal signal-safe emergency writer supplement the system's normal crash handling.

Jetsam and forced termination can end a process without a final record. An abrupt log ending
alone does not prove a crash. Save all files in Diagnostics after a problem, including rotated
files if the app has already been restarted. Matching dSYM files are still needed to symbolize
system crash reports; preserve them for the exact executable under test.

```sh
python tools/analyze_runtime_log.py /path/to/runtime.log
python tools/analyze_runtime_log.py /path/to/runtime.log --json --output report.json
```

The text report currently uses Russian; JSON keys are in English. The analyzer also accepts
older logs and distinguishes initial loading from later severe slowdowns.

## iPhone 15 Pro defaults

On `iPhone16,1` and `iPhone16,2`, a one-time profile sets a 30 FPS target, 0.75 render scale
(960x540), bilinear filtering and the FPS counter. Other settings are preserved. The marker
`iphone15pro-v2.applied` prevents subsequent launches from overwriting menu changes. Users can
then choose other settings normally. This is a starting profile, not a guarantee of frame rate.

## Validation

The integrated runtime was built and linked for arm64 iPhoneOS with Xcode 27, and tested on an
iPhone 15 Pro. The tester reported that the previous slowed/crackling audio disappeared. A
6m54s log contained no Metal errors and a median renderer FPS sample of 38. A separate log
contained four deactivate/activate cycles without persistent GPU stalls or recorded audio
shortages. These are observations from one device, not a controlled FPS comparison or a claim
of stable 60 FPS. Thermal state reached `serious`; long sessions and other devices still need
testing.

A brief low FPS reading after activation is partly affected by the existing renderer counter:
its averaging interval includes inactive time. The slow-frame sampler can likewise include
the preceding pause. This change does not alter either counter, and those readings alone
cannot distinguish a real short hitch from a measurement artifact.

Host checks use the real portable implementations with deterministic SDL/POSIX test doubles:

```sh
python tests/test_log_analysis.py
python tests/run_hotpaths.py --cxx clang++
python tests/run_diagnostics.py --cxx clang++
python tests/run_audio_resampler.py --cxx clang++
```

The C++ runners also accept `--zig /path/to/zig` and explicitly select the host SDK on macOS.
The GitHub workflow runs these checks without game files or generated code. They do not replace
a device build or UIKit/Metal integration testing.

For an existing iOS build configured with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, run
`python tools/ci/apple_preflight.py build-ios/iphoneos/compile_commands.json` to syntax-check
the 13 relevant Apple translation units before rebuilding the generated game units. This
requires the normal dependencies, generated sources and an Apple SDK, following the build guide.

On a device, test sound before and after repeated Control Center/background transitions,
speakers/headphones and audio interruptions. Record longer sessions with the same save, route,
render settings and shader-cache state before drawing performance conclusions.
