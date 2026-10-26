"""Exercise the user-facing error boundary and sunlight diagnostics without network fixtures."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from dng_fixture import write_dng_fixture

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("PHOTO_OPS_BINARY", ROOT / "build/linux/x86_64/debug/photo-ops"))


def write_fixture(path):
    width, height = 96, 64
    field_count = 10
    bits_offset = 8 + 2 + field_count * 12 + 4
    pixels_offset = bits_offset + 6
    fields = [(256, 4, 1, width), (257, 4, 1, height), (258, 3, 3, bits_offset),
              (259, 3, 1, 1), (262, 3, 1, 2), (273, 4, 1, pixels_offset),
              (277, 3, 1, 3), (278, 4, 1, height), (279, 4, 1, width*height*3),
              (284, 3, 1, 1)]
    data = bytearray(b"II" + struct.pack("<HIH", 42, 8, field_count))
    for field in fields:
        data.extend(struct.pack("<HHII", *field))
    data.extend(struct.pack("<IHHH", 0, 8, 8, 8))
    for y in range(height):
        for x in range(width):
            data.extend((160, 146, 130) if 24 <= x < 72 and 20 <= y < 48 else (55, 50, 45))
    path.write_bytes(data)


class CommandLine(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([str(BINARY), *map(str, args)], text=True, capture_output=True)

    def test_bad_input_returns_normal_error(self):
        for args in [("inspect", "missing.cr3"),
                     ("sunlight-enhance", "missing.cr3", "-o", "unused.tif", "--strength", "nan"),
                     ("sunlight-enhance", "missing.cr3", "-o", "unused.tif", "--strength", "3")]:
            result = self.run_cli(*args)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("photo-ops:", result.stderr)
            self.assertNotIn("unexpected failure", result.stderr)

    def test_diagnostics_and_zero_strength(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.tif"
            write_fixture(source)
            result = self.run_cli("sunlight-enhance", source, "-o", root/"enhanced.tif",
                                  "--diagnostics", root/"diagnostics")
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((root/"diagnostics/report.json").read_text())
            self.assertTrue(report["applied"])
            self.assertGreater(report["mean_relative_chroma_gain"], 1)
            self.assertGreater(report["mean_luminance_change_ev"], 0)
            self.assertEqual(report["decision"], "enhanced")
            for name in ["before.jpg", "after.jpg", "mask.tif", "mask.jpg",
                         "detail-before.jpg", "detail-after.jpg"]:
                self.assertGreater((root/"diagnostics"/name).stat().st_size, 0)

            existing = self.run_cli("sunlight-enhance", source, "-o", root/"other.tif",
                                    "--diagnostics", root/"diagnostics")
            self.assertEqual(existing.returncode, 1)
            self.assertFalse((root/"other.tif").exists())

            zero = self.run_cli("sunlight-enhance", source, "-o", root/"zero.tif",
                                "--strength", "0", "--diagnostics", root/"zero")
            self.assertEqual(zero.returncode, 0, zero.stderr)
            self.assertFalse(json.loads((root/"zero/report.json").read_text())["applied"])

    def test_focus_stack_cli_and_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.tif"
            write_fixture(source)
            result = self.run_cli("focus-stack", source, source, "-o", root/"stack.tif",
                                  "--memory-budget-mib", "512", "--tile-size", "32",
                                  "--scratch-directory", root,
                                  "--diagnostics", root/"diagnostics")
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((root/"diagnostics/report.json").read_text())
            self.assertEqual(report["reference_index"], 1)
            self.assertEqual(report["input_origin"], "rendered_rgb")
            self.assertEqual(len(report["frames"]), 2)
            self.assertEqual(report["frames"][0]["a"], 1)
            self.assertLessEqual(report["estimated_peak_bytes"], 512*1024*1024)
            profile = report["profile"]
            self.assertEqual(profile["decoded_frames"], 2)
            self.assertGreater(profile["output_seconds"], 0)
            self.assertEqual(profile["output_bytes"], (root/"stack.tif").stat().st_size)
            self.assertGreater(profile["scratch_peak_file_bytes"], 0)
            self.assertLessEqual(profile["peak_tile_cache_bytes"], report["tile_cache_capacity_bytes"])
            self.assertTrue((root/"stack.tif").is_file())
            self.assertFalse(list(root.glob("photo-ops-focus-*")))
            self.assertTrue((root/"diagnostics/defocus-boundary.tif").is_file())
            self.assertTrue((root/"diagnostics/boundary-source.tif").is_file())
            conflict = root/"conflict"
            conflict.mkdir()
            (conflict/"defocus-boundary.tif").write_bytes(b"preserve-existing-map")
            collision = self.run_cli("focus-stack", source, source, "-o", root/"collision.tif",
                                     "--diagnostics", conflict)
            self.assertEqual(collision.returncode, 1)
            self.assertFalse((root/"collision.tif").exists())
            self.assertEqual((conflict/"defocus-boundary.tif").read_bytes(), b"preserve-existing-map")
            self.assertFalse((conflict/"source-map.tif").exists())
            again = self.run_cli("focus-stack", source, source, "-o", root/"stack.tif")
            self.assertEqual(again.returncode, 1)

    @unittest.skipIf(os.geteuid() == 0, "root bypasses directory write permissions")
    def test_focus_diagnostic_failure_preserves_complete_photo(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root/"source.tif"
            write_fixture(source)
            diagnostics = root/"read-only"
            diagnostics.mkdir(mode=0o555)
            try:
                result = self.run_cli("focus-stack", source, source, "-o", root/"stack.tif",
                                      "--diagnostics", diagnostics)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("complete image published at", result.stderr)
                self.assertIn("diagnostics failed", result.stderr)
                inspected = self.run_cli("inspect", root/"stack.tif")
                self.assertEqual(inspected.returncode, 0, inspected.stderr)
                self.assertFalse((diagnostics/"report.json").exists())
                self.assertFalse(list(diagnostics.iterdir()))
            finally:
                diagnostics.chmod(0o755)

    def test_hdr_cli_and_numeric_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = [root/f"{index}.dng" for index in range(3)]
            for path, exposure in zip(inputs, [(1,400), (1,100), (1,25)]):
                write_dng_fixture(path, exposure)
            result = self.run_cli("hdr", *inputs, "-o", root/"hdr.tif", "--aligned",
                                  "--memory-budget-mib", "512", "--tile-size", "32",
                                  "--scratch-directory", root, "--diagnostics", root/"diagnostics")
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((root/"diagnostics/report.json").read_text())
            self.assertFalse(report["tone_mapping"])
            self.assertEqual(report["reference_index"], 1)
            self.assertGreater(report["maximum"], 1)
            self.assertEqual([row["exposure_scale"] for row in report["frames"]], [4,1,.25])
            self.assertAlmostEqual(sum(row["contribution_fraction"] for row in report["frames"]), 1)
            self.assertEqual(report["raw_context"]["exposure_policy"], "shutter_bracket")
            self.assertEqual(report["profile"]["output_bytes"], (root/"hdr.tif").stat().st_size)
            inspected = self.run_cli("inspect", root/"hdr.tif")
            self.assertEqual(inspected.returncode, 0, inspected.stderr)
            self.assertIn("Rec.2020", inspected.stdout)
            for name in ["confidence.tif", "deghost.tif", "clipping.tif", "low-snr.tif", "deghost-source.tif"]:
                inspected = self.run_cli("inspect", root/"diagnostics"/name)
                self.assertEqual(inspected.returncode, 0, inspected.stderr)
                self.assertIn("measurement:", inspected.stdout)
            self.assertFalse(list(root.glob("photo-ops-hdr-*")))
            focus = self.run_cli("focus-stack", *inputs, "-o", root/"wrong-focus.tif", "--aligned")
            self.assertEqual(focus.returncode, 1)
            self.assertIn("exposure mismatch", focus.stderr)
            self.assertFalse((root/"wrong-focus.tif").exists())
            collision = self.run_cli("hdr", *inputs, "-o", root/"collision.tif", "--aligned", "--diagnostics", root/"diagnostics")
            self.assertEqual(collision.returncode, 1)
            self.assertFalse((root/"collision.tif").exists())
            duplicate = self.run_cli("hdr", inputs[0], inputs[0], inputs[1], "-o", root/"duplicate.tif", "--aligned")
            self.assertEqual(duplicate.returncode, 1)
            self.assertIn("duplicate", duplicate.stderr)
            self.assertFalse((root/"duplicate.tif").exists())

    @unittest.skipIf(os.geteuid() == 0, "root bypasses directory write permissions")
    def test_hdr_diagnostic_failure_keeps_complete_photo(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = [root/f"{index}.dng" for index in range(3)]
            for path, exposure in zip(inputs, [(1,400), (1,100), (1,25)]):
                write_dng_fixture(path, exposure)
            diagnostics = root/"readonly"
            diagnostics.mkdir(mode=0o555)
            try:
                result = self.run_cli("hdr", *inputs, "-o", root/"hdr.tif", "--aligned", "--diagnostics", diagnostics)
                self.assertEqual(result.returncode, 1)
                self.assertIn("complete image published at", result.stderr)
                self.assertEqual(self.run_cli("inspect", root/"hdr.tif").returncode, 0)
                self.assertFalse((diagnostics/"report.json").exists())
            finally:
                diagnostics.chmod(0o755)

    def test_focus_stack_rejects_missing_raw_and_invalid_options(self):
        cases = [
            (("focus-stack", "a.CR3", "b.CR3", "-o", "unused.tif"), "open RAW sequence"),
            (("focus-stack", "a.tif", "-o", "unused.tif"), "at least two"),
            (("focus-stack", "a.tif", "b.tif", "-o", "unused.tif",
              "--strength", "1"), "--strength requires"),
            (("focus-stack", "a.tif", "b.tif", "-o", "unused.tif",
              "--memory-budget-mib", "-1"), "nonnegative integer"),
            (("convert", "a.tif", "-o", "unused.tif", "--aligned"), "require focus-stack"),
        ]
        for args, message in cases:
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(message, result.stderr)
                self.assertNotIn("unexpected failure", result.stderr)


if __name__ == "__main__":
    unittest.main()
