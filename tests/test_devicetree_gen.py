"""Verify generated device-tree data, phandles and invalid input handling."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = ROOT / 'scripts' / 'gen_devicetree.py'


class DevicetreeTests(unittest.TestCase):
    def test_generate_and_use_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'board.dts').write_text('''/dts-v1/;
/ {
    #address-cells = <1>;
    #size-cells = <1>;
    spi: spi@40013000 {
        compatible = "vendor,spi";
        reg = <0x40013000 0x400>;
        flash@0 { compatible = "vendor,flash"; reg = <0>; bus = <&spi>; };
    };
};
''')
            result = subprocess.run([sys.executable, str(GEN), '--input', str(path / 'board.dts'),
                                     '--output', str(path / 'tree.c')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            (path / 'check.c').write_text('''#include <nos/devicetree.h>
#include <string.h>
int main(void) {
    const struct nos_dt_node *n = nos_dt_find_path("/spi@40013000/flash@0");
    uint32_t reg = 123, bus = 0, phandle = 0;
    if (!n || nos_dt_read_u32(n, "reg", 0, &reg) || reg != 0) return 1;
    if (nos_dt_read_u32(n, "bus", 0, &bus)) return 4;
    n = nos_dt_find_path("/spi@40013000");
    if (!n || nos_dt_read_u32(n, "reg", 0, &reg) || reg != 0x40013000) return 2;
    if (nos_dt_read_u32(n, "reg", 2, &reg) == 0) return 3;
    if (nos_dt_read_u32(n, "phandle", 0, &phandle) || bus != phandle) return 5;
    return 0;
}
''')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-I', str(ROOT / 'tests' / 'host' / 'stubs'), '-I', str(ROOT / 'include'),
                            str(path / 'check.c'), str(path / 'tree.c'),
                            str(ROOT / 'drivers/base/devicetree.c'), '-o', str(path / 'check')], check=True)
            subprocess.run([str(path / 'check')], check=True)

    def test_invalid_tree_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'bad.dts').write_text('/dts-v1/; / { bad = <&missing>; };')
            result = subprocess.run([sys.executable, str(GEN), '--input', str(path / 'bad.dts'),
                                     '--output', str(path / 'tree.c')], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((path / 'tree.c').exists())


if __name__ == '__main__':
    unittest.main()
