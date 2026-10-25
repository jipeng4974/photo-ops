"""Regression checks for the extra spacing rule clang-format cannot express."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("photo_format", ROOT / "scripts/format.py")
formatter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(formatter)


class DeclarationSpacing(unittest.TestCase):
    def test_functions_and_global_constants(self):
        source = "void first();\nvoid second();\nconstexpr int count = 3;\nconst int other = 4;\n"
        result = formatter.separate_declarations(source)
        self.assertIn("first();\n\nvoid second", result)
        self.assertIn("count = 3;\n\nconst int other", result)

    def test_local_statements_and_fields_stay_together(self):
        source = "struct Box {\n  int x;\n  int y;\n};\nvoid f() {\n  int x = 1;\n  int y = 2;\n}\n"
        result = formatter.separate_declarations(source)
        self.assertIn("int x;\n  int y;", result)
        self.assertIn("int x = 1;\n  int y = 2;", result)

    def test_aggregate_and_method_declarations(self):
        source = "namespace n {\nconst Box box{1, 2};\nvoid f();\n}\nstruct A {\n  void f();\n  void g();\n};\n"
        result = formatter.separate_declarations(source)
        self.assertIn("box{1, 2};\n\nvoid f", result)
        self.assertIn("void f();\n\n  void g", result)

    def test_comments_and_literals_do_not_change_scope(self):
        source = 'const char *text = "}; {";\n/// Documentation stays with the next declaration.\nvoid f();\n'
        result = formatter.separate_declarations(source)
        self.assertIn('"}; {";\n\n/// Documentation', result)
        self.assertEqual(result, formatter.separate_declarations(result))

    def test_native_contracts_and_idempotence(self):
        source = "int f(int x) pre(x > 0) {\n  contract_assert(x > 0);\n  return x;\n}\nint g();\n"
        result = formatter.separate_declarations(source)
        self.assertIn("return x;\n}\n\nint g", result)
        self.assertEqual(result, formatter.separate_declarations(result))


if __name__ == "__main__":
    unittest.main()
