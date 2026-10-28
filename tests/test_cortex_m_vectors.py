"""Validate linked Cortex-M vectors against the selected STM32 family."""
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
