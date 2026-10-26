"""Black-box benchmark checks using independent tiny Bayer inputs and the native CLI."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from dng_fixture import write_dng_fixture

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('PHOTO_OPS_BINARY', ROOT/'build/linux/x86_64/debug/photo-ops')).resolve()
SCRIPT = ROOT/'scripts/benchmark-focus.py'


class Benchmark(unittest.TestCase):
    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), '--binary', str(BINARY), *map(str, args)],
                              capture_output=True, text=True)

    def test_hdr_and_matrix_measurements_freeze_binary_and_record_originals(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            planes = []
            for focus in ('near', 'far'):
                files = []
                for index, shutter in enumerate(((1,400), (1,100), (1,25))):
                    path = root/f'{focus}-{index}.dng'
                    write_dng_fixture(path, shutter)
                    files.append(str(path))
                planes.append(dict(name=focus, exposures=files))
            manifest = root/'capture.json'
            manifest.write_text(json.dumps(dict(focus_planes=planes)))
            cases = [('hdr', ['--operator', 'hdr', *planes[0]['exposures']], 3),
                     ('matrix', ['--matrix', str(manifest)], 6)]
            for name, descriptor, count in cases:
                output = root/name
                result = self.run_tool(*descriptor, '--output', output, '--aligned',
                                       '--memory-budget-mib', '512', '--tile-size', '32')
                self.assertEqual(result.returncode, 0, result.stderr)
                summary = json.loads((output/'summary.json').read_text())
                self.assertEqual(summary['frames'], count)
                self.assertEqual(summary['exit_code'], 0)
                self.assertGreater(summary['peak_rss_kib'], 0)
                self.assertGreater(summary['wall_seconds'], 0)
                snapshot = output/'photo-ops.binary'
                with snapshot.open('rb') as stream:
                    self.assertEqual(summary['binary_sha256'], hashlib.file_digest(stream, 'sha256').hexdigest())
                self.assertEqual(Path(summary['command'][0]), snapshot)
                self.assertFalse(list((output/'scratch').iterdir()))
                if name == 'matrix':
                    self.assertEqual(summary['workload_kind'], 'focus-exposure-matrix')
                    self.assertEqual(summary['matrix_sha256'], hashlib.sha256(manifest.read_bytes()).hexdigest())
                    self.assertEqual(summary['report']['algorithm'], 'focus-hdr-matrix-v1')
                original = (output/'summary.json').read_bytes()
                collision = self.run_tool(*descriptor, '--output', output)
                self.assertNotEqual(collision.returncode, 0)
                self.assertEqual((output/'summary.json').read_bytes(), original)

    def test_incompatible_descriptors_fail_before_creating_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)/'measurement'
            for args in [[], ['--operator', 'hdr', '--repeat', '3', 'missing.dng'],
                         ['--matrix', 'missing.json', 'extra.dng'],
                         ['--matrix', 'missing.json', '--operator', 'hdr']]:
                result = self.run_tool(*args, '--output', output)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
