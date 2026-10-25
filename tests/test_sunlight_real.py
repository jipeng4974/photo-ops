"""Opt-in regressions for the licensed, hash-pinned landscape RAW fixtures."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('PHOTO_OPS_BINARY', ROOT/'build/linux/x86_64/debug/photo-ops'))


@unittest.skipUnless(os.environ.get('PHOTO_OPS_REAL_FIXTURES') == '1',
                     'set PHOTO_OPS_REAL_FIXTURES=1 after fetching landscape fixtures')
class SunlightLandscapes(unittest.TestCase):
    def test_effect_and_abstention(self):
        manifest = json.loads((ROOT/'tests/data/landscape-manifest.json').read_text())
        with tempfile.TemporaryDirectory(prefix='photo-sunlight-real-') as directory:
            root = Path(directory)
            for i, entry in enumerate(manifest['files']):
                with self.subTest(fixture=entry['path']):
                    source = ROOT/'tests/data/downloads'/entry['path']
                    with source.open('rb') as stream:
                        self.assertEqual(hashlib.file_digest(stream, 'sha256').hexdigest(),
                                         entry['sha256'])
                    diagnostics = root/str(i)
                    result = subprocess.run(
                        [str(BINARY), 'sunlight-enhance', str(source), '--half-size',
                         '-o', str(root/f'{i}.tif'), '--diagnostics', str(diagnostics)],
                        text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    report = json.loads((diagnostics/'report.json').read_text())
                    if 'into-darkness' in source.name:
                        self.assertTrue(report['applied'])
                        self.assertGreater(report['mean_relative_chroma_gain'], 1.05)
                        self.assertLess(report['mean_relative_chroma_gain'], 1.35)
                        self.assertGreater(report['mean_luminance_change_ev'], .08)
                        self.assertLess(report['mean_luminance_change_ev'], .20)
                        self.assertGreater(report['selected_area_fraction'], .04)
                        self.assertLess(report['selected_area_fraction'], .09)
                        self.assertTrue((diagnostics/'detail-before.jpg').exists())
                    else:
                        # Intrinsic bright ice and bright clouds are negative cases,
                        # not reasons to keep searching until some bright patch wins.
                        self.assertFalse(report['applied'])
                        self.assertEqual(report['selected_area_fraction'], 0)
                        self.assertEqual(report['changed_pixel_fraction'], 0)
                        self.assertEqual(report['mean_relative_chroma_gain'], 1)
                        self.assertEqual(report['mean_luminance_change_ev'], 0)


if __name__ == '__main__':
    unittest.main()
