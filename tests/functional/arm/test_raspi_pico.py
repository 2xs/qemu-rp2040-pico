#!/usr/bin/env python3
#
# Functional test that checks the Raspberry Pi Pico machine.
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import QemuSystemTest, wait_for_console_pattern


class RaspiPicoMachine(QemuSystemTest):

    # Minimal Cortex-M0+ raw image linked for XIP at 0x10000000. It contains a
    # vector table and writes "PICO UART OK\n" to UART0 at 0x40034000.
    UART_TEST_BIN = bytes([
        0x00, 0x20, 0x04, 0x20, 0x09, 0x00, 0x00, 0x10,
        0x07, 0x48, 0x08, 0x4a, 0x11, 0x78, 0x00, 0x29,
        0x02, 0xd0, 0x01, 0x60, 0x01, 0x32, 0xf9, 0xe7,
        0xfe, 0xe7, 0x50, 0x49, 0x43, 0x4f, 0x20, 0x55,
        0x41, 0x52, 0x54, 0x20, 0x4f, 0x4b, 0x0a, 0x00,
        0x00, 0x40, 0x03, 0x40, 0x1a, 0x00, 0x00, 0x10,
    ])

    def test_uart0(self):
        self.set_machine('raspi-pico')

        image = self.scratch_file('uart-test.bin')
        with open(image, 'wb') as image_file:
            image_file.write(self.UART_TEST_BIN)

        self.vm.set_console()
        self.vm.add_args('-kernel', image)
        self.vm.launch()

        wait_for_console_pattern(self, 'PICO UART OK')

    def test_flash_file_uart0(self):
        flash = self.scratch_file('flash.bin')
        with open(flash, 'wb') as flash_file:
            flash_file.write(self.UART_TEST_BIN)

        self.set_machine('raspi-pico')
        self.vm.set_machine(f'raspi-pico,flash-file={flash}')
        self.vm.set_console()
        self.vm.launch()

        wait_for_console_pattern(self, 'PICO UART OK')


if __name__ == '__main__':
    QemuSystemTest.main()
