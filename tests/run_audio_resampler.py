"""Compile the actual AX 32->48 kHz function and verify pitch, channel separation and amplitude."""
import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
compilers = parser.add_mutually_exclusive_group()
compilers.add_argument('--cxx', default='clang++')
compilers.add_argument('--zig')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/src/snd/ax_ist.cpp').read_text(encoding='utf-8')
begin = source.index('\tsint32 AXUpsampleLinear32To48(')
end = source.index('\n\tvoid AXTransferSamples(', begin)
function = source[begin:end]
prefix = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
using sint32 = int32_t;
static sint32 _swapEndianS32(sint32 n) { return (sint32)__builtin_bswap32((uint32_t)n); }
'''
test = r'''
int main() {
    constexpr int blocks = 1000, inputFrames = 96, outputFrames = 144;
    sint32 shifted[inputFrames*2], plain[inputFrames*2];
    sint32 outputA[outputFrames*2], outputB[outputFrames*2];
    sint32 historyA[2]{}, historyB[2]{};
    int crossings[2]{}, previous[2]{}, maximumError = 0;
    const double frequencies[] = {1000, 500};
    for (int b = 0; b < blocks; ++b) {
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < inputFrames; ++i) {
                int sample = (int)(12000 * std::sin(2 * 3.141592653589793 * frequencies[c] * (b*inputFrames+i) / 32000));
                shifted[c*inputFrames+i] = _swapEndianS32(sample*256);
                plain[c*inputFrames+i] = _swapEndianS32(sample);
            }
        }
        assert(AXUpsampleLinear32To48(shifted, outputA, historyA, inputFrames, true, 2) == outputFrames);
        assert(AXUpsampleLinear32To48(plain, outputB, historyB, inputFrames, false, 2) == outputFrames);
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < outputFrames; ++i) {
                int a = _swapEndianS32(outputA[c*outputFrames+i]);
                int plain = _swapEndianS32(outputB[c*outputFrames+i]);
                maximumError = std::max(maximumError, std::abs(a-plain));
                assert(std::abs(a) <= 12001);
                if (a > 0 && previous[c] <= 0) ++crossings[c];
                previous[c] = a;
            }
        }
    }
    double duration = (double)blocks*outputFrames/48000;
    for (int c = 0; c < 2; ++c) {
        double measured = crossings[c]/duration;
        assert(std::abs(measured-frequencies[c]) < 1);
        std::cout << "channel " << c << ": " << measured << " Hz at 48000 Hz output; expected " << frequencies[c] << " Hz\n";
    }
    assert(maximumError <= 1);
    std::cout << "Actual AX resampler: duration, pitch, planar channels, endian conversion and amplitude passed.\n";
}
'''
build = root / 'build-tests'
build.mkdir(exist_ok=True)
cpp = build / 'audio_resampler.cpp'
cpp.write_text(prefix + function + test, encoding='utf-8')
exe = build / ('audio_resampler.exe' if sys.platform == 'win32' else 'audio_resampler')
command = [args.zig, 'c++'] if args.zig else [args.cxx]
command += ['-std=c++20', '-O2', '-UNDEBUG', str(cpp), '-o', str(exe)]
if sys.platform == 'darwin':
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    command += ['-isysroot', sdk, '-stdlib=libc++']
subprocess.run(command, check=True)
subprocess.run([str(exe)], check=True)
