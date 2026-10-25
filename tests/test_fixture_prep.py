"""Exercise fixture identity/provenance workflows; dummy bytes are explicitly not camera RAW."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT/'scripts/prepare-r8-fixture.py'
spec = importlib.util.spec_from_file_location('fixture_prep', SCRIPT)
prep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prep)


class FixturePreparation(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.parent = Path(temporary.name)
        self.scene = self.parent/'scene'
        self.scene.mkdir()
        self.names = ['IMG_003.CR3', 'IMG_001.CR3', 'IMG_002.CR3']
        for index, name in enumerate(self.names):
            (self.scene/name).write_bytes(f'dummy test identity, not real RAW {index}'.encode())
        self.metadata = dict(camera='Canon EOS R8', lens='RF85mm F2 Macro IS STM',
                             focal_length_mm=85, aperture=4, iso=100, exposure_time_seconds=.008, capture='tripod',
                             focus_order=['near', 'mid', 'far'], files=self.names,
                             scene_tags=['focus-breathing'], photographer='Test fixture author',
                             public=False, canonical_regression=False, notes='Synthetic file identities only')
        self.write_metadata()

    def write_metadata(self):
        (self.scene/'metadata.json').write_text(json.dumps(self.metadata))

    def register(self):
        return prep.register(self.scene, None, None)

    def manifest(self):
        return json.loads((self.scene/'manifest.json').read_text())

    def test_order_identity_and_honest_validation_status(self):
        status = self.register()
        manifest = self.manifest()
        self.assertEqual(status['raw_decoder_validation'], 'not-run')
        self.assertEqual(manifest['state'], 'registered-not-decoder-validated')
        self.assertEqual([row['path'] for row in manifest['files']], self.names)
        self.assertNotIn(str(self.scene), json.dumps(manifest))
        for row in manifest['files']:
            self.assertEqual(row['sha256'], hashlib.sha256((self.scene/row['path']).read_bytes()).hexdigest())
        self.assertEqual(prep.verify(self.scene/'manifest.json', None)['verified_files'], 3)

    def test_mutation_and_reordered_capture_are_detected(self):
        self.register()
        source = self.scene/self.names[0]
        original = source.read_bytes()
        source.write_bytes(original+b'changed')
        with self.assertRaisesRegex(ValueError, 'content/size mismatch'):
            prep.verify(self.scene/'manifest.json', None)
        source.write_bytes(original)
        manifest = self.manifest()
        manifest['files'].reverse()
        (self.scene/'manifest.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'order disagrees'):
            prep.verify(self.scene/'manifest.json', None)
        manifest['files'].reverse()
        (self.scene/'manifest.json').write_text(json.dumps(manifest))
        self.metadata['iso'] = 200
        self.write_metadata()
        with self.assertRaisesRegex(ValueError, 'metadata changed'):
            prep.verify(self.scene/'manifest.json', None)

    def test_paths_cannot_escape_the_scene(self):
        outside = self.parent/'outside.CR3'
        outside.write_bytes(b'outside file')
        (self.scene/'escape.CR3').symlink_to(outside)
        for name in ('../outside.CR3', str(outside), 'escape.CR3'):
            with self.subTest(path=name):
                self.metadata['files'] = [name, *self.names[1:]]
                self.write_metadata()
                with self.assertRaises(ValueError):
                    self.register()
                self.assertFalse((self.scene/'manifest.json').exists())

    def test_duplicate_alias_empty_and_missing_frames_fail(self):
        (self.scene/'alias.CR3').symlink_to(self.scene/self.names[0])
        for names in ([self.names[0], 'alias.CR3', self.names[2]], [self.names[0], self.names[0], self.names[2]]):
            self.metadata['files'] = names
            self.write_metadata()
            with self.assertRaisesRegex(ValueError, 'unique CR3'):
                self.register()
        self.metadata['files'] = self.names
        self.write_metadata()
        (self.scene/self.names[1]).write_bytes(b'')
        with self.assertRaisesRegex(ValueError, 'empty file'):
            self.register()
        (self.scene/self.names[1]).unlink()
        with self.assertRaises(OSError):
            self.register()

    def test_metadata_and_rights_require_explicit_evidence(self):
        original = self.metadata.copy()
        cases = [dict(photographer=''), dict(iso=True), dict(aperture=0), dict(exposure_time_seconds=float('inf')),
                 dict(focus_order=['near']), dict(scene_tags=[]), dict(public='yes'),
                 dict(public=True, license='CC BY 4.0'), dict(canonical_regression=True),
                 dict(canonical_regression=True, expected_behavior=True)]
        for change in cases:
            with self.subTest(change=change):
                self.metadata = {**original, **change}
                self.write_metadata()
                with self.assertRaises(ValueError):
                    self.register()
        self.metadata = {**original, 'public': True, 'license': 'CC0',
                         'license_url': 'https://creativecommons.org/publicdomain/zero/1.0/',
                         'canonical_regression': True, 'expected_behavior': 'Preserve branch geometry'}
        self.write_metadata()
        self.register()
        self.assertTrue(prep.verify(self.scene/'manifest.json', None)['canonical_regression_declared'])

    def test_manifest_publication_never_overwrites(self):
        self.register()
        manifest = self.scene/'manifest.json'
        original = manifest.read_bytes()
        with self.assertRaises(FileExistsError):
            self.register()
        self.assertEqual(manifest.read_bytes(), original)
        link = self.scene/'dangling.json'
        link.symlink_to(self.scene/'absent.json')
        with self.assertRaises(FileExistsError):
            prep.register(self.scene, link, None)
        self.assertTrue(link.is_symlink())
        self.assertFalse(list(self.scene.glob('.photo-ops-fixture-*')))

    def test_external_manifest_verifies_with_explicit_directory(self):
        output = self.parent/'manifests/scene.json'
        prep.register(self.scene, output, None)
        self.assertEqual(prep.verify(output, self.scene)['verified_files'], 3)
        with self.assertRaises(OSError):
            prep.verify(output, None)

    def test_cli_template_and_order_override_are_checked(self):
        target = self.parent/'new scene'
        first = subprocess.run([sys.executable, str(SCRIPT), 'init', str(target)], capture_output=True, text=True)
        self.assertEqual(first.returncode, 0, first.stderr)
        second = subprocess.run([sys.executable, str(SCRIPT), 'init', str(target)], capture_output=True, text=True)
        self.assertEqual(second.returncode, 1)
        self.assertIn('output already exists', second.stderr)
        with self.assertRaisesRegex(ValueError, 'order differs'):
            prep.register(self.scene, None, list(reversed(self.names)))

    def test_malformed_manifest_metadata_is_rejected(self):
        self.register()
        original = self.manifest()
        changes = [dict(version=True), dict(kind='hdr-matrix'), dict(canonical_regression_declared=True)]
        for change in changes:
            (self.scene/'manifest.json').write_text(json.dumps({**original, **change}))
            with self.assertRaises(ValueError):
                prep.verify(self.scene/'manifest.json', None)
        original['files'][0]['size_bytes'] = True
        (self.scene/'manifest.json').write_text(json.dumps(original))
        with self.assertRaisesRegex(ValueError, 'byte size'):
            prep.verify(self.scene/'manifest.json', None)


if __name__ == '__main__':
    unittest.main()
