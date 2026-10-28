"""Exercise the layer dependency checker with isolated source trees."""
from contextlib import redirect_stderr
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_layer_deps', ROOT / 'scripts/check_layer_deps.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class LayerDepsTests(unittest.TestCase):
    def test_board_headers_are_rejected_in_portable_layers(self):
        old_root = checker.ROOT
        try:
            with tempfile.TemporaryDirectory() as tmp:
                checker.ROOT = Path(tmp)
                (checker.ROOT / 'drivers/block').mkdir(parents=True)
                source = checker.ROOT / 'drivers/block/test.c'
                source.write_text('#include <stdint.h>\n')
                self.assertEqual(checker.main(), 0)
                source.write_text('#include <stdint.h>\n#include <board/board.h>\n')
                output = io.StringIO()
                with redirect_stderr(output):
                    self.assertEqual(checker.main(), 1)
                self.assertIn('drivers/block/test.c:2', output.getvalue())
        finally:
            checker.ROOT = old_root


if __name__ == '__main__':
    unittest.main()
