# TODO: Raspberry Pi Pico 1 / RP2040 QEMU Target

This checklist is the step-by-step plan for integrating a minimal
Raspberry Pi Pico 1 target in QEMU, starting from Alex Bennee's RP2040 RFC
patches and adapting them to the current tree.

## Phase 0: Baseline

- [x] Confirm the working tree is clean or identify unrelated local changes.
- [x] Create a dedicated branch for the Pico/RP2040 work.
- [x] Configure a minimal `arm-softmmu` build.
- [ ] Build the unmodified tree once.
- [x] Record the exact baseline command used for configure/build/test.

Baseline commands used:

- `./configure --target-list=arm-softmmu --disable-docs`
- `ninja -C build qemu-system-arm`
- `build/qemu-system-arm -machine help`
- `build/qemu-system-arm -machine raspi-pico -display none -monitor none -serial none -S -daemonize -pidfile /tmp/rp2040-pico.pid`

## Phase 1: Import and Adapt the RP2040 SoC Skeleton

- [x] Review `/tmp/rp2040-rfc-patches/0001-hw-arm-arm-initial-boilerplate-for-RP2040-SoC.patch`.
- [x] Add `include/hw/arm/rp2040.h`.
- [x] Add `hw/arm/rp2040.c`.
- [x] Add the `RP2040` Kconfig symbol.
- [x] Ensure `RP2040` selects or depends on the required ARMv7-M support.
- [x] Add `rp2040.c` to the current `hw/arm/meson.build` structure.
- [x] Add `CONFIG_RP2040=y` to the relevant default ARM device config if still appropriate.
- [x] Build `arm-softmmu`.
- [x] Fix API drift from the 2022 RFC patch.
- [x] Run the relevant style/checkpatch checks for the new files.

## Phase 2: Add the Raspberry Pi Pico Machine

- [x] Review `/tmp/rp2040-rfc-patches/0002-hw-arm-add-boilerplate-for-machines-based-on-the-RP2.patch`.
- [x] Add `hw/arm/raspi_pico.c`.
- [x] Define the machine as `raspi-pico`.
- [x] Add the `RASPI_PICO` Kconfig symbol.
- [x] Make `RASPI_PICO` select `RP2040`.
- [x] Add `raspi_pico.c` to `hw/arm/meson.build`.
- [x] Add `CONFIG_RASPI_PICO=y` to the relevant default ARM device config if still appropriate.
- [x] Set machine properties for a microcontroller board: no floppy, no CD-ROM, no parallel, no SD card.
- [x] Build `arm-softmmu`.
- [x] Verify `qemu-system-arm -machine help` lists `raspi-pico`.

## Phase 3: Wire the Minimal RP2040 Memory Map

- [x] Review `/tmp/rp2040-rfc-patches/0003-hw-arm-wire-up-memory-from-the-Pico-board-and-the-So.patch`.
- [x] Add the board-to-SoC system-memory link.
- [x] Map internal ROM at `0x00000000`.
- [x] Map XIP flash at `0x10000000`.
- [x] Use a default Pico 1 flash size of `2 MiB`.
- [x] Map SRAM at `0x20000000`.
- [x] Map SRAM bank 4 at `0x20040000`.
- [x] Map SRAM bank 5 at `0x20041000`.
- [x] Decide whether to model one Cortex-M0+ initially or instantiate both with CPU1 powered off.
- [x] Add unimplemented MMIO regions for the RP2040 peripheral ranges needed to avoid silent holes.
- [x] Build `arm-softmmu`.
- [x] Start `raspi-pico` without firmware and confirm machine initialization does not crash.

## Phase 4: Firmware Loading and Initial Execution

- [x] Decide the first boot policy: direct `-kernel` load into XIP at `0x10000000`.
- [x] Add firmware loading through `armv7m_load_kernel()`.
- [x] Confirm ELF loading behavior for images linked at `0x10000000`.
- [ ] Confirm raw binary loading behavior.
- [x] Create or obtain a tiny bare-metal test firmware that loops.
- [x] Launch QEMU with the test firmware.
- [x] Confirm the CPU reaches guest code instead of failing during reset/vector fetch.
- [x] Document any temporary boot behavior that differs from real RP2040 boot ROM flow.

Current temporary boot behavior:

- QEMU installs a tiny synthetic boot ROM at `0x00000000`.
- The synthetic ROM uses a fixed SRAM stack top and branches to the reset
  handler from the XIP vector table at `0x10000004`.
- This is only a bring-up path; it is not a faithful RP2040 mask ROM model.

## Phase 5: Boot ROM Strategy

- [ ] Review `/tmp/rp2040-rfc-patches/0004-pc-bios-add-pipico-mask-rom-upstream.patch`.
- [ ] Review `/tmp/rp2040-rfc-patches/0005-hw-arm-add-mask-boot-ROM-logic.patch`.
- [ ] Do not import the extracted binary `pc-bios/pipico.rom` as-is for an upstreamable path.
- [x] Decide whether the initial implementation uses an empty/simplified ROM or requires a user-supplied ROM.
- [ ] If using a ROM image, make loading optional and document the file name and search path.
- [x] If using a simplified ROM, document exactly what it does and does not emulate.
- [x] Build `arm-softmmu`.
- [x] Verify direct XIP boot still works.

## Phase 6: Minimal UART0 Console

- [x] Identify the best existing QEMU UART model or decide that a small RP2040 UART shim is needed.
- [x] Map UART0 at `0x40034000`.
- [x] Connect UART0 to QEMU serial chardev infrastructure.
- [x] Implement enough registers for polling transmit.
- [ ] Return stable documented values for unimplemented UART status bits.
- [x] Create or obtain a bare-metal hello-world firmware using UART0.
- [x] Launch with a host serial backend.
- [x] Confirm hello-world text is visible.
- [x] Build `arm-softmmu`.
- [x] Run style/checkpatch checks for the UART changes.

## Phase 7: First Automated Test

- [ ] Choose the test framework: prefer `tests/functional` for a full-system boot test.
- [ ] Add or reference a tiny UART hello-world firmware fixture.
- [ ] Add a test that launches `qemu-system-arm -machine raspi-pico`.
- [ ] Load the firmware with `-kernel`.
- [ ] Wait for the expected UART text.
- [ ] Add a timeout that fails clearly on boot hangs.
- [ ] Run the new test locally.
- [ ] Ensure the test is skipped cleanly if an optional toolchain or fixture is unavailable.

## Phase 8: XIP Flash Backing

- [ ] Replace the first simple XIP ROM region if needed.
- [ ] Model the default erased state as `0xff`.
- [ ] Keep the default flash size at `2 MiB`.
- [ ] Add a raw host backing file option or board property.
- [ ] Load initial flash contents from the raw file.
- [ ] Map the flash contents executable at `0x10000000`.
- [ ] Verify guest reads from XIP see the raw file contents.
- [ ] Verify firmware still executes from XIP.

## Phase 9: Minimal Flash Programming Model

- [ ] Decide where the command model belongs: SSI/QSPI controller, flash device, or temporary board-level model.
- [ ] Implement write enable.
- [ ] Implement read status.
- [ ] Implement page program with 256-byte pages.
- [ ] Implement sector erase with 4096-byte sectors.
- [ ] Enforce NOR programming as `old & new`.
- [ ] Reject or document unsupported commands.
- [ ] Define behavior for out-of-range erase/program requests.
- [ ] Add tests for successful erase/program/readback.
- [ ] Add tests for programming without write enable.
- [ ] Add tests for attempting to change bits from `0` back to `1` without erase.

## Phase 10: Flash Busy and XIP Access Semantics

- [ ] Decide the behavior for XIP reads while flash is busy.
- [ ] Document the chosen behavior.
- [ ] Implement the chosen behavior.
- [ ] Add a test for XIP access while busy if busy timing/state is modeled.

## Phase 11: Flash Persistence

- [ ] Ensure flash modifications are written back to the raw host file.
- [ ] Add a first-run test that erases/programs flash.
- [ ] Add a second-run test that reads the persisted bytes.
- [ ] Verify persistence across separate QEMU invocations.
- [ ] Document the raw backing file workflow.

## Phase 12: Minimal Clock, Reset, Watchdog, and Timer Stubs

- [ ] Identify which registers the initial test firmware actually touches.
- [ ] Add minimal clock/reset/watchdog/timer behavior only when needed.
- [ ] Return documented constant values for simple read-only stubs.
- [ ] Avoid claiming full RP2040 fidelity.
- [ ] Add regression tests for any behavior required by firmware boot.

## Phase 13: Documentation

- [ ] Add user documentation for the `raspi-pico` machine.
- [ ] Document RAM size.
- [ ] Document flash size.
- [ ] Document XIP address `0x10000000`.
- [ ] Document SRAM address `0x20000000`.
- [ ] Document the `-kernel` launch flow.
- [ ] Document UART console usage.
- [ ] Document flash backing file usage when available.
- [ ] Document known limitations.
- [ ] Add short developer notes explaining simplified versus faithful RP2040 behavior.

## Phase 14: Patch Series Preparation

- [ ] Split the work into small reviewable commits.
- [ ] Keep SoC skeleton, machine, memory map, firmware loading, UART, tests, flash, persistence, and docs separate where practical.
- [ ] Ensure each commit builds independently.
- [ ] Run checkpatch on each commit.
- [ ] Run the relevant functional/qtest tests.
- [ ] Prepare the first submission as RFC if the model is still minimal.
- [ ] State clearly that this is not complete RP2040 emulation.

## Phase 15: Post-Integration Roadmap

- [ ] Add the second Cortex-M0+ properly.
- [ ] Add SIO and inter-core FIFO.
- [ ] Improve timer fidelity.
- [ ] Improve watchdog/reset behavior.
- [ ] Improve SSI/QSPI fidelity.
- [ ] Support a more faithful boot ROM flow.
- [ ] Expand Pico SDK compatibility.
- [ ] Add PIO.
- [ ] Add DMA.
- [ ] Add USB.
- [ ] Add broader peripheral coverage: SPI, I2C, PWM, ADC.

## Minimal Success Criteria

- [x] `qemu-system-arm -machine help` lists `raspi-pico`.
- [x] QEMU can load a bare-metal firmware with `-kernel`.
- [x] Guest code executes from XIP at `0x10000000`.
- [x] Guest code can use SRAM at `0x20000000`.
- [x] UART output is visible on the host.
- [ ] Guest code can read XIP flash contents.
- [ ] Guest code can erase and program at least one flash sector/page.
- [ ] Flash changes persist in a raw host file across two QEMU runs.
