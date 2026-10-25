"""Compile actual module consumers to verify the pinned library's discard diagnostics."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ResultUsage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='photo-ops-result-usage-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.environment = {**os.environ, 'LC_ALL': 'C'}
        cls.compiler = shlex.split(os.environ.get('CXX', 'g++'))
        cls.flags = ['-std=c++26', '-fmodules', '-fcontracts',
                     '-fcontract-evaluation-semantic=quick_enforce']
        for module in ('core/error', 'image/image', 'color/color', 'image/pyramid',
                       'focus_stack/focus_stack'):
            result = subprocess.run(
                [*cls.compiler, *cls.flags, '-x', 'c++', '-c',
                 str(ROOT / f'src/{module}.cppm'), '-o', f'{Path(module).name}.o'],
                cwd=cls.directory, env=cls.environment, text=True, capture_output=True)
            if result.returncode:
                raise AssertionError(result.stderr)

    def compile_body(self, body):
        source = self.directory / 'consumer.cpp'
        source.write_text('''import photo_ops.error;
import photo_ops.image;
import photo_ops.color;
import photo_ops.pyramid;
import photo_ops.focus_stack;
namespace p = photo_ops;
p::result<int> value() { return 42; }
p::result<void> status() { return {}; }
void consume() {
''' + body + '\n}\n')
        return subprocess.run(
            [*self.compiler, *self.flags, '-Werror=unused-result', '-fsyntax-only', str(source)],
            cwd=self.directory, env=self.environment, text=True, capture_output=True)

    def test_discarded_result_value_and_void_are_diagnosed(self):
        for body in ('value();', 'status();',
                     'value().transform([](int v) { return v + 1; });',
                     'status().and_then([] { return status(); });'):
            with self.subTest(body=body):
                result = self.compile_body(body)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('-Werror=unused-result', result.stderr)

    def test_checked_and_explicitly_discarded_results_compile(self):
        result = self.compile_body('''
  auto number = value().transform([](int v) { return v + 1; });
  if (!number) return;
  auto done = status();
  if (!done) return;
  (void)value();
  (void)status();
''')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_registration_helpers_are_not_exported(self):
        for name in ('p::align_focus_stack', 'p::detail::align_focus_stack'):
            with self.subTest(name=name):
                result = self.compile_body(f'(void){name}({{}}, {{}});')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('align_focus_stack', result.stderr)
                self.assertRegex(result.stderr, r'not a member|not been declared|not declared')

    def test_ordinary_image_and_math_results_can_be_discarded(self):
        result = self.compile_body('''
  p::image<p::linear_rgb> image({4, 4});
  image.size();
  image.view();
  image.clone();
  image.view().stride();
  image.view().storage();
  image.view().row(0);
  image.view().at(0, 0);
  image.view().subview({0, 0, 2, 2});
  p::image<p::linear_rgb>::can_hold({4, 4});
  p::extent{4, 4}.valid();
  p::srgb_to_linear(0.5f);
  p::linear_to_srgb(0.5f);
  p::luminance({0.1f, 0.2f, 0.3f});
  p::to_oklab({0.1f, 0.2f, 0.3f});
  p::from_oklab({0.5f, 0.1f, 0.1f});
  p::luminance_image(image.view());
  p::image<p::gray> gray({4, 4});
  p::downsample(gray.view());
  p::gaussian_levels(gray.view(), 2);
''')
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
