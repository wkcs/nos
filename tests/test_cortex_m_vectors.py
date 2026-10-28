"""Validate linked Cortex-M vectors against the selected STM32 family."""
import re
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def vectors_and_symbols(board):
    elf = ROOT / 'build' / board / 'nos.elf'
    with tempfile.TemporaryDirectory() as directory:
        raw = Path(directory) / 'vectors.bin'
        subprocess.run(['arm-none-eabi-objcopy', '-O', 'binary',
                        '--only-section=.isr_vector', str(elf), str(raw)], check=True)
        data = raw.read_bytes()
    symbols = {}
    for line in subprocess.check_output(['arm-none-eabi-nm', str(elf)], text=True).splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    return struct.unpack('<' + 'I' * (len(data) // 4), data), symbols


class VectorTests(unittest.TestCase):
    def test_soc_early_init_is_linked(self):
        for board in ('nk60-v2', 'qemu-stm32', 'wl-001', 'zj-v3', 'nutiny-nuc472'):
            with self.subTest(board=board):
                _, symbols = vectors_and_symbols(board)
                self.assertIsNotNone(symbols.get('soc_early_init'))

    def test_firmware_has_no_unresolved_reset_symbol(self):
        for board in ('nk60-v2', 'qemu-stm32', 'wl-001', 'zj-v3'):
            with self.subTest(board=board):
                elf = ROOT / 'build' / board / 'nos.elf'
                undefined = subprocess.check_output(
                    ['arm-none-eabi-nm', '--undefined-only', str(elf)], text=True)
                self.assertNotIn('Reset_Handler', undefined)

    def test_stm32f4_vectors_match_vendor_irq_numbers(self):
        header = (ROOT / 'soc/stm32f4/inside-include/stm32f4xx.h').read_text()
        start = header.index('typedef enum IRQn')
        f405_start = header.index('#if defined (STM32F40_41xxx)', start)
        f405_end = header.index('#if defined (STM32F427_437xx)', f405_start)
        f401_start = header.index('#if defined (STM32F401xx) || defined (STM32F411xE)', start)
        f401_end = header.index('#if defined (STM32F411xE)', f401_start)
        common = header[start:f405_start]
        for board, definitions in (('qemu-stm32', header[f405_start:f405_end]),
                                   ('nk60-v2', header[f401_start:f401_end])):
            vectors, symbols = vectors_and_symbols(board)
            entries = re.findall(r'\b(\w+)_IRQn\s*=\s*(\d+)', common + definitions)
            for name, irq in entries:
                with self.subTest(board=board, irq=irq, name=name):
                    symbol = symbols.get(name + '_IRQHandler')
                    self.assertIsNotNone(symbol)
                    self.assertEqual(vectors[16 + int(irq)] & ~1, symbol & ~1)

    def test_stm32f4_part_specific_interrupts(self):
        f405, symbols = vectors_and_symbols('qemu-stm32')
        f401, _ = vectors_and_symbols('nk60-v2')
        for irq, handler in ((19, 'CAN1_TX_IRQHandler'), (39, 'USART3_IRQHandler'),
                             (74, 'OTG_HS_EP1_OUT_IRQHandler')):
            with self.subTest(irq=irq):
                self.assertIsNotNone(symbols.get(handler))
                self.assertEqual(f405[16 + irq] & ~1, symbols[handler] & ~1)
                self.assertEqual(f401[16 + irq], 0)
        self.assertEqual(len(f405), 16 + 82)
        self.assertEqual(len(f401), 16 + 85)

    def test_stm32f1_usb_handler_is_connected(self):
        for board in ('wl-001', 'zj-v3'):
            with self.subTest(board=board):
                vectors, symbols = vectors_and_symbols(board)
                self.assertIsNotNone(symbols.get('USB_LP_CAN1_RX0_IRQHandler'))
                self.assertEqual(vectors[16 + 20] & ~1,
                                 symbols['USB_LP_CAN1_RX0_IRQHandler'] & ~1)

    def test_vector_table_has_family_specific_length(self):
        for board, entries in (('wl-001', 16 + 43), ('zj-v3', 16 + 60),
                               ('nutiny-nuc472', 16 + 142)):
            with self.subTest(board=board):
                vectors, _ = vectors_and_symbols(board)
                self.assertEqual(len(vectors), entries)

    def test_uart_and_dma_irq_slots(self):
        for board, dma_irq, dma_handler in (
            ('wl-001', 14, 'DMA1_Channel4_IRQHandler'),
            ('zj-v3', 14, 'DMA1_Channel4_IRQHandler'),
            ('qemu-stm32', 70, 'DMA2_Stream7_IRQHandler'),
            ('nk60-v2', 70, 'DMA2_Stream7_IRQHandler'),
        ):
            with self.subTest(board=board):
                vectors, symbols = vectors_and_symbols(board)
                self.assertEqual(vectors[16 + 37] & ~1, symbols['USART1_IRQHandler'] & ~1)
                self.assertEqual(vectors[16 + dma_irq] & ~1, symbols[dma_handler] & ~1)


if __name__ == '__main__':
    unittest.main()
