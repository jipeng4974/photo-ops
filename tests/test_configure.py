"""Verify dependency selection without changing the real xmake configuration."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Configure(unittest.TestCase):
    def configure(self, mode=None, overrides=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # A brew executable is deliberately present even in the default-path test.
            for name, code in {
                "brew": "print('/optional/brew')",
                "xmake": "import json, os, sys; print(json.dumps([sys.argv[1:], os.environ.get('PKG_CONFIG_PATH')]))",
            }.items():
                script = root / name
                script.write_text(f"#!{sys.executable}\n{code}\n")
                script.chmod(0o755)
            env = {k: v for k, v in os.environ.items()
                   if k not in ('CC', 'CXX', 'PHOTO_OPS_DEPS', 'PKG_CONFIG_PATH')}
            env['PATH'] = f"{root}:/usr/bin:/bin"
            env['PKG_CONFIG_PATH'] = '/explicit/pkgconfig'
            if mode is not None:
                env['PHOTO_OPS_DEPS'] = mode
            env.update(overrides or {})
            return subprocess.run(['bash', str(ROOT/'scripts/configure.sh'), '-m', 'debug'],
                                  env=env, text=True, capture_output=True)

    def test_system_is_default_even_with_brew_installed(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)
        args, pkg_path = json.loads(result.stdout)
        self.assertIn('--cc=gcc', args)
        self.assertIn('--cxx=g++', args)
        self.assertEqual(pkg_path, '/explicit/pkgconfig')

    def test_brew_is_explicit_and_compiler_overrides_are_respected(self):
        result = self.configure('brew', {'CC': '/custom/gcc', 'CXX': '/custom/g++'})
        self.assertEqual(result.returncode, 0, result.stderr)
        args, pkg_path = json.loads(result.stdout)
        self.assertIn('--cxx=/custom/g++', args)
        self.assertIn('--cc=/custom/gcc', args)
        self.assertTrue(pkg_path.startswith('/optional/brew/opt/libraw/'))
        self.assertTrue(pkg_path.endswith('/explicit/pkgconfig'))

    def test_invalid_dependency_mode_is_rejected(self):
        result = self.configure('unknown')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('PHOTO_OPS_DEPS must be system or brew', result.stderr)


if __name__ == '__main__':
    unittest.main()
