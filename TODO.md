# TODO: Raspberry Pi Pico 1 / RP2040 QEMU Target

This checklist is the step-by-step plan for integrating a minimal
Raspberry Pi Pico 1 target in QEMU, starting from Alex Bennee's RP2040 RFC
patches and adapting them to the current tree.

## RFC Lineage and Integration Policy

- [x] Treat Alex Bennee's 2022 RP2040/Pico RFC series in
  `/tmp/rp2040-rfc-patches` as the starting point for this work.
- [x] Record which RFC patches were adapted, copied, or deliberately deferred.
- [x] Adapt the RFC SoC skeleton, Pico machine, and memory map to the current
  QEMU tree instead of importing stale APIs verbatim.
- [x] Keep the RFC `pc-bios/pipico.rom` image available locally for bring-up
  experiments.
- [ ] Reconcile the RFC mask ROM loading logic with the current synthetic XIP
  boot path.
- [ ] Prefer using the RFC implementation as-is when it still fits current
  QEMU APIs; otherwise debug and document the required adaptation.

Current RFC integration status:

- RFC patch 0001 is adapted as the current `RP2040` SoC skeleton, with current
  Meson/Kconfig wiring and later UART/XIP extensions.
- RFC patch 0002 is adapted as the `raspi-pico` machine.
- RFC patch 0003 is adapted for the memory map. The main addresses are kept,
  but the flash is now owned by a minimal RP2040 XIP/SSI device and defaults
  to the official Pico 1 flash size of 2 MiB.
- RFC patch 0004 is copied as `pc-bios/pipico.rom` for local bring-up. It is
  treated as a useful reference artifact, not as a final upstream boot ROM
  provenance answer.
- RFC patch 0005 has been reviewed but is not the active boot path yet. The
  current model keeps a synthetic boot ROM so direct XIP tests remain stable;
  the next boot-ROM step is to integrate or adapt the RFC mask ROM loading and
  debug the extra RP2040 blocks the real ROM requires.

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
- [x] Add firmware reset registration through `armv7m_load_kernel()`.
- [ ] Restore or confirm ELF loading behavior for images linked at `0x10000000`.
- [x] Confirm raw binary loading behavior.
- [x] Create or obtain a tiny bare-metal test firmware that loops.
- [x] Launch QEMU with the test firmware.
- [x] Confirm the CPU reaches guest code instead of failing during reset/vector fetch.
- [x] Document any temporary boot behavior that differs from real RP2040 boot ROM flow.

Current temporary boot behavior:

- QEMU installs a tiny synthetic boot ROM at `0x00000000`.
- `-kernel` is currently treated as a raw XIP image loaded into the emulated
  flash storage. The `armv7m_load_kernel()` helper is still used to register
  reset handling, not to load the image bytes.
- The synthetic ROM uses a fixed SRAM stack top, sets `VTOR` to the XIP vector
  table, and branches to the reset handler from `0x10000004`.
- This is only a bring-up path; it is not a faithful RP2040 mask ROM model.

## Phase 5: Boot ROM Strategy

- [x] Review `/tmp/rp2040-rfc-patches/0004-pc-bios-add-pipico-mask-rom-upstream.patch`.
- [x] Review `/tmp/rp2040-rfc-patches/0005-hw-arm-add-mask-boot-ROM-logic.patch`.
- [x] Do not import the extracted binary `pc-bios/pipico.rom` as-is for an upstreamable path.
- [x] Copy the RFC `pc-bios/pipico.rom` image locally so it is available for bring-up experiments.
- [x] Decide whether the initial implementation uses an empty/simplified ROM or requires a user-supplied ROM.
- [ ] If using a ROM image, make loading optional and document the file name and search path.
- [x] If using a simplified ROM, document exactly what it does and does not emulate.
- [x] Build `arm-softmmu`.
- [x] Verify direct XIP boot still works.

Current boot ROM strategy note:

- The RFC ROM image is present locally and listed with the QEMU BIOS blobs.
- The active boot path is still the synthetic boot ROM described in phase 4.
- The RFC mask ROM loader from patch 0005 should be integrated or adapted next
  behind a deliberate boot-ROM policy, then debugged against the minimal SoC
  model instead of discarded.

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

- [x] Choose the test framework: prefer `tests/functional` for a full-system boot test.
- [x] Add or reference a tiny UART hello-world firmware fixture.
- [x] Add a test that launches `qemu-system-arm -machine raspi-pico`.
- [x] Load the firmware with `-kernel`.
- [x] Wait for the expected UART text.
- [x] Add a timeout that fails clearly on boot hangs.
- [x] Run the new test locally.
- [x] Ensure the test is skipped cleanly if an optional toolchain or fixture is unavailable.

Current automated test note:

- The first functional test embeds a tiny raw Cortex-M0+ UART firmware directly
  in the test source, so no optional toolchain or external fixture is required.

## Phase 8: XIP Flash Backing

- [x] Replace the first simple XIP ROM region if needed.
- [x] Model the default erased state as `0xff`.
- [x] Keep the default flash size at `2 MiB`.
- [x] Add a raw host backing file option or board property.
- [x] Load initial flash contents from the raw file.
- [x] Map the flash contents executable at `0x10000000`.
- [x] Verify guest reads from XIP see the raw file contents.
- [x] Verify firmware still executes from XIP.

Current XIP backing note:

- `raspi-pico` exposes `flash-file=/path/to/flash.bin` as a raw initial XIP
  image. Missing bytes are initialized to erased NOR state, `0xff`.
- Guest programming and erase now go through the minimal RP2040 XIP/SSI model.
  Host writeback and persistence are intentionally deferred to phase 11.

## Phase 9: Minimal Flash Programming Model

- [x] Decide where the command model belongs: SSI/QSPI controller, flash device, or temporary board-level model.
- [x] Implement write enable.
- [x] Implement read status.
- [x] Implement page program with 256-byte pages.
- [x] Implement sector erase with 4096-byte sectors.
- [x] Enforce NOR programming as `old & new`.
- [x] Reject or document unsupported commands.
- [x] Define behavior for out-of-range erase/program requests.
- [x] Add tests for successful erase/program/readback.
- [ ] Add tests for programming without write enable.
- [ ] Add tests for attempting to change bits from `0` back to `1` without erase.

Current flash command model note:

- The command model lives in a minimal RP2040 XIP/SSI device, mapped at the
  XIP window, XIP control base, and `XIP_SSI_BASE`.
- Unsupported commands are ignored.
- Out-of-range erase/program commands have no effect. If they consumed write
  enable state, write enable is cleared and the flash does not become busy.

## Phase 10: Flash Busy and XIP Access Semantics

- [x] Decide the behavior for XIP reads while flash is busy.
- [x] Document the chosen behavior.
- [x] Implement the chosen behavior.
- [x] Add a test for XIP access while busy if busy timing/state is modeled.

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

- [x] Add user documentation for the `raspi-pico` machine.
- [x] Document RAM size.
- [x] Document flash size.
- [x] Document XIP address `0x10000000`.
- [x] Document SRAM address `0x20000000`.
- [x] Document the `-kernel` launch flow.
- [x] Document UART console usage.
- [x] Document flash backing file usage when available.
- [x] Document known limitations.
- [x] Add short developer notes explaining simplified versus faithful RP2040 behavior.

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
- [x] Guest code can read XIP flash contents.
- [x] Guest code can erase and program at least one flash sector/page.
- [ ] Flash changes persist in a raw host file across two QEMU runs.
