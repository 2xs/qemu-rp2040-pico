.. SPDX-License-Identifier: GPL-2.0-or-later

Raspberry Pi Pico board (``raspi-pico``)
========================================

The ``raspi-pico`` machine models a minimal Raspberry Pi Pico 1 board based
on the RP2040 microcontroller.  The current model is intended for bare-metal
bring-up and tests that execute code from the RP2040 external flash XIP
window.

RFC lineage
-----------

This machine starts from Alex Bennee's 2022 RP2040/Pico RFC patch series.
The RP2040 SoC skeleton, Pico machine, and memory map are adapted from that
series to the current QEMU tree.  The RFC ``pc-bios/pipico.rom`` image is
kept in the tree for bring-up experiments.

RFC patch 0005's mask ROM loading logic is adapted for the current machine:
``pipico.rom`` is used by default when no direct ``-kernel`` image, explicit
``-bios`` image, or raw ``flash-file`` is supplied.  QEMU still uses a small
synthetic boot ROM for direct XIP bring-up, so existing firmware tests remain
stable.

Supported devices
-----------------

 * Two Cortex-M0+ cores. With QEMU's synthetic ROM, core 1 starts in ROM and
   waits for the SDK FIFO launch sequence; with an external ROM image, core 1
   is currently kept powered off.
 * 16 KiB boot ROM window
 * 264 KiB SRAM
 * 2 MiB external flash contents mapped through the XIP window
 * Minimal clock generator and crystal oscillator registers
 * Minimal PLL_SYS and PLL_USB registers
 * Minimal reset controller registers
 * UART0 console

Boot options
------------

For direct bring-up, a firmware image can be loaded into the XIP window with
``-kernel``.  ELF images linked at ``0x10000000`` and Pico 1 UF2 images are
accepted, with raw images loaded at ``0x10000000`` as a fallback:

.. code-block:: bash

  $ qemu-system-arm -machine raspi-pico -kernel firmware.bin -serial stdio

The machine also accepts a raw initial flash image:

.. code-block:: bash

  $ qemu-system-arm -machine raspi-pico,flash-file=flash.bin -serial stdio

Bytes not provided by the raw flash image are initialized to the NOR erased
state, ``0xff``.

If both ``flash-file`` and ``-kernel`` are specified, the raw flash file is
loaded first, then the ``-kernel`` image is overlaid into the emulated XIP
flash.  The complete emulated flash image is written back to the raw file, so
a later run with only ``flash-file`` restarts from the overlaid image.
Successful guest sector erase and page program commands are also written back
to the raw file.

Pico UF2 images can be converted to this raw flash format with:

.. code-block:: bash

  $ scripts/uf2-to-flash.py firmware.uf2 flash.bin

An RP2040 boot ROM image can be supplied explicitly with ``-bios``:

.. code-block:: bash

  $ qemu-system-arm -machine raspi-pico -bios pipico.rom -serial stdio

The file name is resolved through QEMU's BIOS search path, the same mechanism
used by other machines for firmware blobs.  The RFC ``pipico.rom`` image is
installed as a QEMU BIOS blob for local bring-up.  If no ``-kernel``,
``-bios`` or raw ``flash-file`` is supplied, ``raspi-pico`` uses
``pipico.rom`` by default.  Direct XIP bring-up keeps using the synthetic boot
ROM described above, and explicit ``-bios`` still overrides the default.

Mask ROM bring-up tracing
-------------------------

The real mask ROM path can be explored with QEMU's unimplemented-device log:

.. code-block:: bash

  $ qemu-system-arm -machine raspi-pico,flash-file=flash.bin \
      -bios pipico.rom -display none -serial none \
      -d unimp,guest_errors -D rp2040-bios-mmio.log

The RP2040 model names unimplemented MMIO blocks in the log and includes the
absolute address, register offset, access size and write value.  The XIP/SSI
register block also logs APB register accesses through the same ``unimp`` log
mask, without logging every normal XIP instruction fetch.

At the current level of emulation, the RFC ``pipico.rom`` image reaches its
reset handler and progresses through the first clock setup loops.  The earlier
tight polling loop on ``rp2040.clocks`` offset ``0x44`` (``0x40008044``)
and the XOSC stability poll on ``rp2040.xosc`` offset ``0x04``
(``0x40024004``) now complete.  The reset-done poll on ``rp2040.resets``
offset ``0x08`` (``0x4000c008``) also completes.  The current observed tight
polling loop on ``rp2040.pll_sys`` offset ``0x00`` (``0x40028000``) also
completes.  The ROM then switches ``clk_sys`` to the PLL path, writes
watchdog scratch registers, and clears ``XIP_CTRL.CTRL.EN`` through the
atomic clear alias at ``0x14003000``.  The USB controller DPRAM is backed by
the documented 4 KiB RAM window at ``0x50100000``.  ``USBCTRL_REGS`` has a
shallow register-store model for the registers touched by the ROM, including
the RP2040 atomic aliases.  ``SYSINFO`` returns stable chip/platform values,
and ``SYSCFG`` stores the processor NMI/configuration registers touched by the
ROM.  ``PROC0_NMI_MASK`` reroutes connected interrupt sources to the
Cortex-M0+ NMI input, and ``MEMPOWERDOWN`` disables ROM, SRAM bank and USB
DPRAM windows by returning memory transaction errors.  ``VREG_AND_CHIP_RESET``
exposes the voltage-regulator, brown-out detector and chip reset status
registers with stable shallow behaviour.  ``TBMAN.PLATFORM`` reports the
documented ASIC platform bit.  ``SIO`` implements the core ID, the user and
QSPI GPIO output/output-enable registers, 8-entry inter-core FIFOs,
``VLD``/``RDY``/``ROE``/``WOF`` FIFO status, proc0/proc1 FIFO IRQ outputs, and
hardware spinlock claim/release semantics.  Core1 is instantiated. With the
synthetic ROM, it starts at reset, reads ``SIO_CPUID``, and waits in ROM for
the Pico SDK FIFO sequence ``{0, 0, 1, VTOR, SP, PC}``, echoing each received
word.  After a valid sequence, the synthetic ROM installs the supplied
``VTOR`` and main stack pointer, then branches to the supplied entry point.
With an external ROM image, proc1 remains powered off until the faithful core1
ROM path is modeled.
``PSM`` exposes the force-on, force-off, watchdog-select and done registers
touched by the Pico SDK core1 reset path; ``FRCE_OFF_PROC1`` is stored,
reflected in ``DONE``, and used to hold or release proc1.  The SIO divider and
interpolator datapaths remain future work.

A local Pico SDK smoke test using ``multicore_launch_core1()`` has been used
to validate this synthetic ROM core1 launch path.  That SDK build is configured
to use compiler implementations for bit, memory, float and double helpers,
because the synthetic ROM does not yet expose the RP2040 boot ROM function
table ABI.  The in-tree functional tests keep the resulting coverage
self-contained by reproducing the SDK FIFO launch sequence without depending
on the SDK.

Clock and XOSC model
--------------------

The RP2040 datasheet describes clock generator ``SELECTED`` registers as
one-hot status registers for glitchless muxes, and notes that software should
poll them until a source switch completes.  See datasheet pages 203 to 216.
The current QEMU model returns stable one-hot selected values immediately and
updates QEMU ``Clock`` outputs for ``clk-ref``, ``clk-sys``, ``clk-peri``,
``clk-usb``, ``clk-adc`` and ``clk-rtc``.  It models frequencies and software
visible register state, not analog transition latency.

The crystal oscillator model follows the XOSC programmer-visible behaviour
described by the datasheet: XOSC starts disabled, firmware writes the enable
code to ``CTRL``, and then polls ``STATUS.STABLE`` until the oscillator is
usable.  See datasheet pages 217 to 220.  QEMU asserts ``STABLE``
immediately once XOSC is enabled and awake, keeps ``BADWRITE`` sticky until
cleared, and implements the documented ``STARTUP``, ``DORMANT`` and
``COUNT`` registers at the level needed by early boot.

The ring oscillator model follows the programmer-visible register layout
described by the datasheet: ``CTRL``, ``FREQA``, ``FREQB``, ``DORMANT``,
``DIV``, ``PHASE``, ``STATUS``, ``RANDOMBIT`` and ``COUNT``.  See datasheet
pages 221 to 227.  QEMU models a stable nominal ROSC and updates a QEMU
``Clock`` output from the visible enable/dormant/divider state.  ``COUNT`` is
derived from QEMU virtual time rather than CPU cycles; in normal execution
this follows elapsed host time, while in ``icount`` mode it follows QEMU's
deterministic virtual clock.  The model does not emulate analog frequency
variation with process, voltage or temperature.

The QSPI IO bank model implements the documented IO_QSPI register layout for
the six QSPI pins.  It stores each pin's ``CTRL`` register, returns stable
zero ``STATUS`` values, implements shallow interrupt enable/force/status
registers, and supports the RP2040 atomic aliases.  This is enough for boot
firmware to configure the QSPI pin muxing around the XIP/SSI controller.  It
does not emulate pad electrical behaviour or serial flash transfers; those
belong to the pad and XIP/SSI models.

Reset controller model
----------------------

The RP2040 datasheet describes the reset controller at ``0x4000c000`` with
``RESET``, ``WDSEL`` and ``RESET_DONE`` registers.  ``RESET`` holds a
peripheral in reset while its bit is set, and ``RESET_DONE`` reports that the
peripheral's registers are ready once reset is deasserted.  See datasheet
pages 175 to 177.

The current QEMU model stores ``RESET`` and ``WDSEL`` for documented bits
0..24, implements the RP2040 atomic alias windows, and derives
``RESET_DONE`` immediately as the inverse of ``RESET`` for those bits.  It
does not yet propagate resets into the individual peripheral models or model
reset completion delays.

VREG and chip reset model
-------------------------

The RP2040 datasheet describes the shared ``VREG_AND_CHIP_RESET`` register
window at ``0x40064000`` with ``VREG``, ``BOD`` and ``CHIP_RESET`` registers.
``CHIP_RESET`` records chip-level reset sources: power-on/brown-out, RUN pin,
and Rescue Debug Port.  See datasheet pages 157 to 158 and 167.

The current QEMU model stores the writable ``VREG`` and ``BOD`` fields,
reports ``VREG.ROK`` as stable when the regulator is enabled and not in high
impedance mode, and stores the software-visible Rescue Debug Port flag in
``CHIP_RESET``.  Watchdog reset cause is reported by the watchdog block's
``REASON`` register; it is not reflected in ``CHIP_RESET`` because the
documented ``CHIP_RESET`` source fields do not include watchdog reset.

TBMAN model
-----------

The RP2040 datasheet describes ``TBMAN`` as a testbench manager used during
chip development simulations.  On real hardware it only exposes a
``PLATFORM`` register indicating that the platform is ASIC; this is duplicated
by ``SYSINFO.PLATFORM``.  See datasheet pages 309 to 310.

The current QEMU model implements this real-chip subset and returns
``TBMAN.PLATFORM.ASIC`` set.  It deliberately does not expose testbench
simulation controls, because those controls would imply a simulation
environment outside the RP2040 SoC model.

PSM model
---------

The RP2040 datasheet describes the power-on state machine at ``0x40010000``
with ``FRCE_ON``, ``FRCE_OFF``, ``WDSEL`` and ``DONE`` registers.  The Pico
SDK uses ``FRCE_OFF.PROC1`` in ``multicore_reset_core1()`` to hold core 1 off
and then release it before the ROM FIFO launch protocol.  See datasheet pages
179 to 182.

The current QEMU model stores the documented bits 0..16, implements the
RP2040 atomic alias windows, and derives ``DONE`` immediately as the inverse
of ``FRCE_OFF``.  On reset, ``FRCE_OFF`` is clear.  Setting
``FRCE_OFF.PROC1`` powers off proc1 in the QEMU model; clearing it powers
proc1 back on at the ROM reset vector.

PLL model
---------

The RP2040 datasheet describes ``PLL_SYS`` and ``PLL_USB`` at ``0x40028000``
and ``0x4002c000``.  Each PLL exposes ``CS``, ``PWR``, ``FBDIV_INT`` and
``PRIM`` registers; firmware powers the PLL, waits for ``CS.LOCK``, and then
enables the post dividers.  The documented output frequency is
``(FREF / REFDIV) * FBDIV / (POSTDIV1 * POSTDIV2)``.  See datasheet pages
228 to 233.

The current QEMU model is intentionally shallow.  It stores the visible
registers, implements atomic alias writes, reports ``CS.LOCK`` immediately
when the PLL core is powered, and publishes a calculated QEMU ``Clock``
output.  This does not change instruction execution speed directly.  In QEMU,
``Clock`` objects describe the modeled hardware clock tree; TCG execution
rate is not a cycle-accurate function of the guest PLL.  The RP2040 clock
generator model still uses fixed PLL_SYS/PLL_USB frequencies, so dynamic PLL
output wiring is left for a later fidelity step.

RP2040 flash and XIP model
--------------------------

The RP2040 datasheet describes a 16 KiB mask ROM at ``0x00000000``, 264 KiB
of SRAM starting at ``0x20000000``, and external flash accessed through the
QSPI execute-in-place hardware.  See the RP2040 datasheet pages 120 to 122.

The flash is not directly attached as an ordinary parallel memory.  System
bus reads to the 16 MiB XIP window starting at ``0x10000000`` are translated
by the XIP hardware into external serial flash transfers.  The XIP block also
contains a 16 KiB cache and several aliases with different cache behaviour.
See datasheet pages 122 to 124.  The current QEMU model implements the main
XIP window needed by firmware execution and leaves cache timing and cache
aliases for later work.

The RP2040 XIP path is backed by the SSI controller.  The datasheet describes
the SSI as a Synopsys DW_apb_ssi controller connected to the QSPI pins and
forming part of the XIP block.  It can be configured to issue common serial
flash read sequences, including the standard ``0x03`` read command with a
24-bit address.  See datasheet pages 567 to 569.

For software-driven flash operations, firmware programs the SSI through its
APB register interface at ``XIP_SSI_BASE``.  The important registers for the
initial emulation are ``CTRLR0``, ``CTRLR1``, ``SSIENR``, ``SER``, ``BAUDR``,
``SR`` and the data register window beginning at ``DR0``.  The data register
window feeds the transmit FIFO on writes and pops the receive FIFO on reads.
See datasheet pages 597 to 602.

Flash programming policy
------------------------

The emulation should model flash programming through the RP2040 XIP/SSI path,
not as a board-private back door.  The minimal command set is:

 * ``0x06`` write enable
 * ``0x05`` read status
 * ``0x03`` read
 * ``0x02`` page program
 * ``0x20`` sector erase

The flash contents follow NOR semantics:

 * the erased state is ``0xff``;
 * programming can only clear bits, equivalent to ``old & new``;
 * page program is limited to 256-byte pages;
 * sector erase operates on 4096-byte sectors.

Unsupported flash commands are currently ignored.  Out-of-range page program
and sector erase commands have no effect.  If such a command consumed write
enable state, the emulation clears write enable and does not enter the busy
state.

The datasheet notes that software must consider XIP cache coherence around
flash programming operations, and describes ROM routines that reconfigure the
SSI for erase/program flows before restoring a slow XIP read configuration.
It also notes that, between parts of that call sequence, the SSI is not in a
state where it can handle XIP accesses.  See datasheet pages 122 to 124 and
the boot ROM flash routine discussion on pages 134 to 135.

QEMU therefore uses the following deterministic policy: while an emulated
flash page program or sector erase is in progress, any access through the XIP
memory window to the same flash produces a bus error.  On the Cortex-M0+,
unsuitable instruction fetches or faulting memory accesses are reported via
HardFault; the RP2040 datasheet describes the Cortex-M0+ default memory map
and HardFault behaviour on pages 71 to 72.  This is an emulation policy chosen
to make incorrect execute-from-XIP-while-programming behaviour visible and
testable.  It is not intended to model precise flash timing.

UART0 model
-----------

The RP2040 datasheet states that each UART instance is based on ARM PrimeCell
UART PL011 revision r1p5, with 32-byte transmit and receive FIFOs.  It also
states that PL011 modem mode and IrDA mode are not supported by RP2040.  See
datasheet pages 417 to 419.

The current QEMU model therefore wires UART0 at ``0x40034000`` to QEMU's
existing PL011 device.  The register list and flag register layout match the
RP2040 UART programmer's model: ``UARTDR`` is at offset ``0x000``,
``UARTRSR/UARTECR`` at ``0x004`` and ``UARTFR`` at ``0x018``.  See datasheet
pages 429 to 431.

For the initial console use case, the documented stable status behaviour is:

 * ``UARTFR.TXFE`` and ``UARTFR.RXFE`` follow QEMU PL011 FIFO state.
 * ``UARTFR.TXFF`` and ``UARTFR.RXFF`` follow QEMU PL011 FIFO fullness.
 * ``UARTFR.BUSY`` is not modeled with RP2040 transmission timing.
 * ``UARTFR.RI``, ``UARTFR.DCD`` and ``UARTFR.DSR`` are treated as absent
   modem-status inputs and remain deasserted.
 * ``UARTFR.CTS`` has no GPIO-backed CTS input yet and remains deasserted
   unless a future RP2040 UART shim connects it to the GPIO model.

This is sufficient for polling transmit firmware that waits for ``TXFF`` to
clear before writing ``UARTDR``.  A dedicated RP2040 UART wrapper can be added
later if firmware needs GPIO-backed CTS/RTS flow control, precise ``BUSY``
timing, or stricter masking of unsupported PL011 modem/IrDA features.

Known limitations
-----------------

 * Core 0 runs normally.  Core 1 starts in the synthetic ROM, echoes the
   SDK-compatible FIFO launch sequence, installs the provided ``VTOR``/stack,
   and branches to the provided entry point.  When an external mask ROM is
   supplied, core 1 is kept powered off until that ROM path is modeled.
 * UART0 currently uses QEMU's PL011 model directly, with the RP2040
   compatibility policy documented above.
 * The XIP cache, streaming FIFO and detailed timing are not yet modeled.  The
   XIP control and SSI APB register blocks do handle the RP2040 atomic
   ``XOR``/``SET``/``CLR`` aliases.
 * ``IO_QSPI`` stores pin-control and interrupt registers but does not emulate
   the electrical QSPI pads or a separate serial bus.
 * The ROSC model exposes stable register behaviour and a nominal clock.  It
   does not model analog frequency variation or true entropy from
   ``RANDOMBIT``.
 * The boot ROM flow is still a bring-up path and is not yet a faithful
   RP2040 mask ROM execution model.  The synthetic ROM supports the direct
   boot2/application launch path and the core1 FIFO launch sequence, but not
   the RP2040 boot ROM function table used by default SDK bit/mem/float/double
   helper implementations.
 * USB, PIO, DMA and most peripherals are not yet implemented.  USB DPRAM is
   present as RAM and ``USBCTRL_REGS`` stores register state, but USB
   packet-level behavior is not modeled.
 * ``SYSINFO`` and ``SYSCFG`` expose the documented register layout used by
   early firmware.  ``PROC0_NMI_MASK`` is wired for interrupt sources routed
   through the RP2040 IRQ shim, currently including UART0, and
   ``MEMPOWERDOWN`` makes powered-off ROM, SRAM bank and USB DPRAM windows
   return memory transaction errors.  ``DBGFORCE`` is stored but not connected
   to an SWD/debug fabric model.
 * ``VREG_AND_CHIP_RESET`` stores the voltage-regulator and brown-out detector
   control fields and exposes stable chip reset status.  Analog regulator and
   brown-out behaviour is not modeled.
 * ``TBMAN`` exposes only the documented real-chip ``PLATFORM`` register.
 * ``PSM`` stores force-on, force-off and watchdog-select bits.  ``DONE`` is
   derived immediately from ``FRCE_OFF``; analog power sequencing delays are
   not modeled.
 * The watchdog models ``CTRL``, ``LOAD``, ``REASON``, ``SCRATCH`` and
   ``TICK``, including ``CTRL.TRIGGER`` and the RP2040-E1 double-decrement
   behaviour.  Debug pause inputs are stored but not connected to a debug
   fabric model.
 * The TIMER block models the microsecond counter, ``ALARM0`` through
   ``ALARM3``, ``ARMED``, ``DBGPAUSE``, ``PAUSE`` and the interrupt
   registers.  Alarm outputs are connected to RP2040 IRQs 0 through 3.  The
   counter advances on QEMU virtual time rather than CPU-cycle timing, and
   debug pause inputs have no external debug-fabric side effects.
