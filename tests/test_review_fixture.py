"""Opt-in review-tool checks; image analysis dependencies remain outside native/CLI requirements."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PYTHON = os.environ.get('PHOTO_OPS_REVIEW_PYTHON')
BINARY = Path(os.environ.get('PHOTO_OPS_BINARY', ROOT/'build/linux/x86_64/debug/photo-ops')).resolve()


@unittest.skipUnless(PYTHON, 'set PHOTO_OPS_REVIEW_PYTHON to a NumPy/Pillow/tifffile environment')
class ReviewFixture(unittest.TestCase):
    def run_tool(self, output, *args):
        return subprocess.run([PYTHON, str(ROOT/'scripts/review-focus-exposure.py'),
                               '--output', str(output), '--width', '128', '--height', '96', *map(str, args)],
                              text=True, capture_output=True)

    def test_deterministic_measurements_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('first', 'second'):
                result = self.run_tool(root/name)
                self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads((root/'first/capture.json').read_text())
            provenance = json.loads((root/'first/provenance.json').read_text())
            self.assertTrue(provenance['not_real_r8_capture'])
            for filename, field in [('review-generator.py.txt', 'script_sha256'),
                                    ('dng-fixture-writer.py.txt', 'fixture_writer_sha256'),
                                    ('capture.json', 'capture_manifest_sha256')]:
                self.assertEqual(hashlib.sha256((root/'first'/filename).read_bytes()).hexdigest(), provenance[field])
            self.assertEqual(len(manifest['focus_planes']), 2)
            self.assertEqual(len(provenance['files']), 6)
            for record in provenance['files']:
                first = root/'first'/record['path']
                second = root/'second'/record['path']
                self.assertEqual(first.read_bytes(), second.read_bytes())
                self.assertEqual(hashlib.sha256(first.read_bytes()).hexdigest(), record['sha256'])
            original = (root/'first/capture.json').read_bytes()
            again = self.run_tool(root/'first')
            self.assertNotEqual(again.returncode, 0)
            self.assertEqual((root/'first/capture.json').read_bytes(), original)
            invalid = self.run_tool(root/'invalid', '--width', '127')
            self.assertEqual(invalid.returncode, 2)
            self.assertFalse((root/'invalid').exists())

    def test_native_decode_and_fixed_curve_review(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)/'review'
            result = self.run_tool(root, '--binary', BINARY, '--memory-budget-mib', '512', '--tile-size', '32')
            self.assertEqual(result.returncode, 0, result.stderr)
            metrics = json.loads((root/'metrics.json').read_text())['measurements']
            self.assertLess(metrics['matrix']['all_relative_mae'],
                            min(metrics['near-hdr']['all_relative_mae'], metrics['far-hdr']['all_relative_mae']))
            self.assertGreater(metrics['matrix']['maximum'], 1)
            for name in ('truth', 'matrix', 'near-hdr', 'far-hdr', 'contact-sheet',
                         'hdr-confidence', 'hdr-clipping', 'hdr-low-snr', 'hdr-deghost', 'combined-confidence'):
                self.assertEqual((root/f'{name}.png').read_bytes()[:8], b'\x89PNG\r\n\x1a\n')
            summary = json.loads((root/'matrix/summary.json').read_text())
            self.assertEqual(summary['frames'], 6)
            self.assertEqual(summary['exit_code'], 0)
            self.assertIn('x/(x+0.18)', (root/'review.md').read_text())


if __name__ == '__main__':
    unittest.main()
