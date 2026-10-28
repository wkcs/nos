# NuTiny-SDK-NUC472

This initial port targets the NUC472HI8AE (512 KiB APROM, 64 KiB SRAM).
It runs from APROM at 0x00000000 using the 22.1184 MHz internal oscillator.
UART0 is a polling console at 115200 8N1: PG.1 is RX and PG.2 is TX.
Connect a 3.3 V USB-to-UART adapter and common ground to use the console;
the Nu-Link debug USB connection is not the console. The PB.10 I/O LED blinks
at a 500 ms interval. The default configuration starts the
NLK service with log and shell capabilities, but has no communication bearer;
remote NLK access is not yet available. USB device, USB host/OTG, and external
interrupts are not yet supported. RamFS is used as the root filesystem; FatFs
and the RAM disk are disabled to fit the available SRAM.

The `flash` target supports current upstream OpenOCD (tested with commit
`cb0c224`). This version uses HIDAPI for
Nu-Link and has been tested on macOS with no `sudo`. Older OpenOCD releases
such as 0.12.0 do not include the NUC472 M4 target configuration. Build and
flash with an appropriate `openocd` on `PATH`:

```sh
cmake --preset nutiny-nuc472
cmake --build --preset nutiny-nuc472
cmake --build --preset nutiny-nuc472 --target flash
```

The flash target programs and verifies `nos.hex` in APROM, then resets the MCU.
It overwrites the existing program. To select another installation, configure
`NOS_OPENOCD` and, if needed, `NOS_OPENOCD_SCRIPTS` to its executable and
matching script directory.
