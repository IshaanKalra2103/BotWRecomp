import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.analyze_runtime_log import analyze, report

class LogAnalysisTests(unittest.TestCase):
    def test_v1_memory_and_low_fps_without_claiming_crash(self):
        result = analyze('[ 2.003] ios: footprint 216 MB, available before jetsam 3159 MB\n'
                         '[ 5.664] [gpu] 0.0 fps, 7 draws/frame\n'
                         '[ 20.000] [gpu] 30.0 fps, 4000 draws/frame\n'
                         '[ 295.087] [gpu] 1.0 fps, 4523 draws/frame\n'
                         '[ 332.330] ios: footprint 3143 MB, available before jetsam 232 MB')
        self.assertEqual(result['summary']['peak_footprint_mib'], 3143)
        self.assertEqual(result['summary']['minimum_headroom_mib'], 232)
        self.assertEqual(result['summary']['first_low_fps_seconds'], 295.087)
        self.assertFalse(result['normal_exit_recorded'])
        self.assertIn('не доказательство jetsam', report(result))

    def test_v2_audio_lifecycle_and_error(self):
        result = analyze('[ 3.001234] #22 t123 lifecycle: inactive, reason Control Center\n'
                         '[ 4.000000] #23 t123 audio: level=-40.0 dBFS queued=60.0 ms max_producer_gap=35.5 ms shortage_requests=2 resets=1\n'
                         '[ 5.000000] #24 t123 metal: COMMAND BUFFER FAILED code=1\n'
                         '[ 6.000000] #25 t123 session: normal main loop exit')
        self.assertEqual(result['lifecycle'][0]['state'], 'inactive')
        self.assertEqual(result['summary']['max_audio_gap_ms'], 35.5)
        self.assertEqual(len(result['gpu_errors']), 1)
        self.assertTrue(result['normal_exit_recorded'])

    def test_unprefixed_emergency_and_empty_file(self):
        self.assertEqual(len(analyze('FATAL SIGNAL: signal=0xb')['fatal_records']), 1)
        self.assertIsNone(analyze('')['summary']['minimum_headroom_mib'])
        self.assertIn('не различить', report(analyze('')))

    def test_legacy_sample_counter_detects_slow_production(self):
        result = analyze('[ 10.000] audio: -40.0 dBFS over the last 10 s\n'
                         '[ 36.710] audio: -40.0 dBFS over the last 10 s')
        self.assertAlmostEqual(result['legacy_audio_intervals'][0]['percent_realtime'], 37.43916, places=3)
        self.assertIn('26.710', report(result))

if __name__ == '__main__': unittest.main()
