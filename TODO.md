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
- [x] Reconcile the RFC mask ROM loading logic with the current synthetic XIP
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
- RFC patch 0005 is adapted as the default mask ROM policy when no direct
  `-kernel`, explicit `-bios`, or raw `flash-file` image is supplied. The
  current model keeps a synthetic boot ROM for direct `-kernel` bring-up, but
  that ROM now expects and executes a boot2 block at `0x10000000` before
  launching the application vectors at `0x10000100`. Explicit `-bios` remains
  an override.
- Mask ROM bring-up tracing is available with `-d unimp,guest_errors` and
  logs named RP2040 MMIO accesses, including absolute addresses. The first
  observed blocker, repeated polling of `rp2040.clocks` at `0x40008044`, is
  resolved by the minimal clock model. The later blocker on `rp2040.resets`
  at `0x4000c008` is resolved by the minimal reset controller. The later
  `0x14003000` HardFault is resolved by modelling the XIP control atomic
  aliases. `USBCTRL_DPRAM` is now mapped as the documented 4 KiB USB data
  RAM. `USBCTRL_REGS` has a shallow register-store model with RP2040 atomic
  aliases; USB packet-level behavior is still out of scope.

## Phase 0: Baseline

- [x] Confirm the working tree is clean or identify unrelated local changes.
- [x] Create a dedicated branch for the Pico/RP2040 work.
- [x] Configure a minimal `arm-softmmu` build.
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
- [x] Restore or confirm ELF loading behavior for images linked at `0x10000000`.
- [x] Support Pico 1 UF2 images through `-kernel`.
- [x] Confirm raw binary loading behavior.
- [x] Create or obtain a tiny bare-metal test firmware that loops.
- [x] Launch QEMU with the test firmware.
- [x] Confirm the CPU reaches guest code instead of failing during reset/vector fetch.
- [x] Document any temporary boot behavior that differs from real RP2040 boot ROM flow.

Current temporary boot behavior:

- QEMU installs a tiny synthetic boot ROM at `0x00000000`.
- `-kernel` is loaded into the emulated XIP flash storage through the RP2040
  XIP loader, which accepts ELF images, Pico 1 UF2 images, and raw images as
  a fallback. The `armv7m_load_kernel()` helper is still used to register
  reset handling.
- The synthetic ROM uses a fixed SRAM stack top, copies the 256-byte boot2
  block from `0x10000000` to SRAM at `0x20041f00`, calls it, then sets `VTOR`
  to the application vector table at `0x10000100` and branches to the
  application reset handler.
- This is only a bring-up path; it is not a faithful RP2040 mask ROM model.

## Phase 5: Boot ROM Strategy

- [x] Review `/tmp/rp2040-rfc-patches/0004-pc-bios-add-pipico-mask-rom-upstream.patch`.
- [x] Review `/tmp/rp2040-rfc-patches/0005-hw-arm-add-mask-boot-ROM-logic.patch`.
- [x] Do not import the extracted binary `pc-bios/pipico.rom` as-is for an upstreamable path.
- [x] Copy the RFC `pc-bios/pipico.rom` image locally so it is available for bring-up experiments.
- [x] Decide whether the initial implementation uses an empty/simplified ROM or requires a user-supplied ROM.
- [x] If using a ROM image, make loading optional and document the file name and search path.
- [x] If using a simplified ROM, document exactly what it does and does not emulate.
- [x] Build `arm-softmmu`.
- [x] Verify direct XIP boot still works.

Current boot ROM strategy note:

- The RFC ROM image is present locally and listed with the QEMU BIOS blobs.
- A ROM image can be supplied explicitly with `-bios`, and is resolved through
  QEMU's BIOS search path.
- `-bios pipico.rom` now loads through QEMU's ROM loader so the Cortex-M reset
  path sees the boot ROM vector table correctly.
- RFC patch 0005 is adapted with a deliberate boot-ROM policy: if no
  `-kernel`, `-bios`, or `flash-file` is supplied, the Pico machine uses
  `pipico.rom` from QEMU's BIOS search path. Direct XIP bring-up keeps the
  synthetic ROM described in phase 4, and explicit `-bios` still overrides
  the default.

Current mask ROM trace finding:

- With a raw flash probe image, the real ROM starts at reset vector `0x000000ef`.
- Early accesses touch `sio`, `clocks`, `syscfg`, `vreg_and_chip_reset`,
  `watchdog`, `resets`, `tbman`, `rosc`, and later `sysinfo`.
- The first tight polling loop was on `clocks` offset `0x44`
  (`0x40008044`). This now completes with the minimal clock model.
- The ROM then enables XOSC and polls `xosc` offset `0x04`
  (`0x40024004`). This now completes with the minimal XOSC model.
- The ROM then polls `resets` offset `0x08` (`0x4000c008`) for reset-done
  state. This now completes with the minimal reset controller.
- The ROM then polls `pll_sys` offset `0x00` (`0x40028000`), after writes
  to `CS`, `FBDIV_INT`, `PRIM`, and the atomic alias for `PWR`.
- This PLL lock loop now completes with the minimal PLL model. The ROM then
  switches `clk_sys` to the PLL path, touches watchdog scratch registers, and
  clears `XIP_CTRL.CTRL.EN` through the atomic clear alias at `0x14003000`.
  This now completes with the minimal XIP control alias model.

## Phase 6: Minimal UART0 Console

- [x] Identify the best existing QEMU UART model or decide that a small RP2040 UART shim is needed.
- [x] Map UART0 at `0x40034000`.
- [x] Connect UART0 to QEMU serial chardev infrastructure.
- [x] Implement enough registers for polling transmit.
- [x] Return stable documented values for unimplemented UART status bits.
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
- The UF2 path is also tested: the functional test builds a minimal Pico 1 UF2
  image, passes it directly with `-kernel`, and also checks that
  `flash-file=... -kernel firmware.uf2` overlays the UF2 contents into the
  emulated XIP flash.

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
- If `flash-file` and `-kernel` are both supplied, the raw file is loaded
  first, then the `-kernel` image overlays the in-memory XIP flash contents.
- `scripts/uf2-to-flash.py` converts Pico UF2 files into this raw flash image
  format for host-side testing.
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
- [x] Add tests for programming without write enable.
- [x] Add tests for attempting to change bits from `0` back to `1` without erase.

Current flash command model note:

- The command model lives in a minimal RP2040 XIP/SSI device, mapped at the
  XIP window, XIP control base, and `XIP_SSI_BASE`.
- Unsupported commands are ignored.
- Functional tests cover successful erase/program/readback, ignored page
  program without write enable, and NOR `old & new` programming semantics.
- Out-of-range erase/program commands have no effect. If they consumed write
  enable state, write enable is cleared and the flash does not become busy.

## Phase 10: Flash Busy and XIP Access Semantics

- [x] Decide the behavior for XIP reads while flash is busy.
- [x] Document the chosen behavior.
- [x] Implement the chosen behavior.
- [x] Add a test for XIP access while busy if busy timing/state is modeled.

## Phase 11: Flash Persistence

- [x] Ensure flash modifications are written back to the raw host file.
- [x] Make `flash-file=flash.bin -kernel firmware.{elf,uf2,bin}` update the
  raw flash file with the overlaid `-kernel` contents at startup, so a later
  run with only `flash-file=flash.bin` restarts from the same programmed
  image.
- [x] Add a first-run test that erases/programs flash.
- [x] Add a second-run test that reads the persisted bytes.
- [x] Verify persistence across separate QEMU invocations.
- [x] Document the raw backing file workflow.

Current persistence note:

- When `flash-file` is supplied, QEMU writes back the complete emulated flash
  image, `flash-size` bytes, after `-kernel` overlay and after successful
  guest sector erase or page program commands.
- Functional tests cover a persisted `-kernel` UF2 overlay and a persisted
  guest erase/program sequence observed by a second QEMU invocation.

## Phase 12: Minimal Clock, Reset, Watchdog, and Timer Stubs

- [x] Identify which clock/XOSC registers the RFC mask ROM polls first.
- [x] Add a minimal RP2040 clocks device for clock generator registers needed
  by early boot.
- [x] Add a minimal RP2040 XOSC device with documented reset, enable,
  bad-write, dormant, startup, count and stable-status behavior.
- [x] Move CPU and UART clock wiring onto QEMU `Clock` outputs from the
  RP2040 clock model.
- [x] Avoid claiming full RP2040 clock fidelity.
- [x] Add a minimal reset-controller model for `RESET_DONE`.
- [x] Add minimal PLL_SYS/PLL_USB register models for `CS.LOCK` polling.
- [x] Add RP2040 watchdog countdown, software trigger, reason, scratch, and
  tick-generator behavior.
- [x] Add regression tests for watchdog behavior required by firmware boot.
- [x] Add a minimal RP2040 TIMER model with a virtual-time microsecond
  counter, four one-shot alarms, IRQ delivery, and a functional alarm test.

Current clock/reset bring-up note:

- The RP2040 clock model follows QEMU's `Clock` framework: registers update
  stable clock output frequencies, while guest time and instruction execution
  remain managed by QEMU's existing virtual clock machinery.
- `CLK_*_SELECTED` returns stable one-hot values immediately instead of
  modeling glitchless mux transition latency.
- XOSC follows the datasheet-visible programming model: reset-disabled,
  enable code `0xfab`, disable code `0xd1e`, `BADWRITE` sticky until cleared,
  and `STABLE` asserted immediately once enabled and awake.
- The RFC `pipico.rom` no longer blocks on `CLK_SYS_SELECTED` or XOSC
  `STATUS_STABLE`.
- The reset controller exposes `RESET`, `WDSEL`, and `RESET_DONE`. `RESET`
  resets all documented blocks at QEMU reset, `WDSEL` is stored, and
  `RESET_DONE` is derived immediately as `~RESET` for bits 0..24, without
  modeling reset propagation delay.
- The RFC `pipico.rom` no longer blocks on `RESET_DONE` or `pll_sys` offset
  `0x00` (`0x40028000`).
- `ROSC` now exposes the documented control, frequency, dormant, divider,
  phase, status, random-bit, and count registers. Its `COUNT` register is
  derived from QEMU virtual time rather than CPU cycles, and the model
  publishes a nominal QEMU `Clock` output.
- `IO_QSPI` now stores QSPI pin `CTRL` and interrupt registers, returns stable
  zero pin `STATUS` values, and implements RP2040 atomic aliases. It is an
  IO-control model only, not a serial flash bus or pad-electrical model.
- The PLL model is intentionally shallow: it stores `CS`, `PWR`,
  `FBDIV_INT`, and `PRIM`, applies the RP2040 atomic aliases, reports
  `CS.LOCK` immediately when the PLL is powered, and publishes a calculated
  QEMU `Clock` output. The current RP2040 clock generator still uses fixed
  PLL_SYS/PLL_USB frequencies, so the PLL model does not yet affect CPU
  execution speed or clock mux output.
- The RFC `pipico.rom` no longer blocks on `PLL_SYS` lock or on the earlier
  XIP control alias access. The USB data DPRAM is now backed by RAM and
  `USBCTRL_REGS` stores the registers touched by the boot ROM, including
  atomic aliases. Full USB signaling and packet handling remain future work.
- `VREG_AND_CHIP_RESET` now exposes shallow `VREG`, `BOD`, and `CHIP_RESET`
  registers with RP2040 atomic aliases. `VREG.ROK` is reported stable when
  the regulator is enabled, and watchdog reset cause remains in
  `WATCHDOG.REASON` rather than `CHIP_RESET`.
- `TBMAN` now returns the documented real-chip `PLATFORM.ASIC` bit. QEMU does
  not expose testbench simulation controls.
- `SYSINFO` now returns stable `CHIP_ID`, `PLATFORM`, and `GITREF_RP2040`
  values. `SYSCFG` now stores the documented processor NMI/configuration,
  input synchronizer bypass, debug-force, and memory-powerdown registers.
  `PROC0_NMI_MASK` reroutes connected IRQ sources to the Cortex-M0+ NMI input,
  and `MEMPOWERDOWN` powers off ROM, SRAM bank, and USB DPRAM windows by
  returning memory transaction errors. `DBGFORCE` remains stored without SWD
  debug side effects.
- The watchdog block now models `CTRL`, `LOAD`, `REASON`, `SCRATCH`, and
  `TICK`, schedules a QEMU virtual-time timeout from `clk_ref / TICK.CYCLES`,
  applies the RP2040-E1 double-decrement behavior, and uses QEMU's watchdog
  action path for both timer expiry and `CTRL.TRIGGER`.
- The TIMER block now exposes the 64-bit microsecond counter registers,
  `ALARM0..3`, `ARMED`, `DBGPAUSE`, `PAUSE`, `INTR`, `INTE`, `INTF`, and
  `INTS`. Alarm outputs are wired to RP2040 IRQs 0..3 and tested through
  NVIC delivery. The counter uses QEMU virtual time rather than CPU-cycle
  timing, and pause/debug side effects remain minimal.
- The SIO block now provides `CPUID`, user GPIO and QSPI `GPIO_HI`
  output/output-enable registers with set/clear/xor operations, 8-entry
  inter-core FIFOs, FIFO `VLD`/`RDY`/`ROE`/`WOF` status, proc0/proc1 FIFO IRQ
  outputs, and simple hardware spinlock claim/release semantics. With the
  synthetic ROM, core1 starts at reset and waits in ROM for the SDK FIFO launch
  sequence. With an external mask ROM, core1 remains powered off until the
  faithful core1 ROM path is modeled. The divider and interpolators remain
  future work.

## Phase 13: SIO Multicore Groundwork

- [x] Make `SIO_CPUID` depend on QEMU's current guest vCPU context.
- [x] Add 8-entry inter-core FIFO storage for both directions.
- [x] Implement FIFO `VLD`, `RDY`, `ROE`, and `WOF` status semantics.
- [x] Implement `FIFO_WR` push to the peer core and `FIFO_RD` pop from the
  current core.
- [x] Add FIFO IRQ outputs for proc0/proc1.
- [x] Route only proc0 FIFO IRQ while the model still has one Cortex-M0+.
- [x] Add a functional test for core0-visible SIO FIFO status and sticky bits.
- [ ] Add qtest coverage that can exercise both FIFO directions without
  requiring a second Cortex-M0+ to execute guest code.

Current multicore groundwork note:

- SIO uses QEMU's `current_cpu` thread-local guest vCPU pointer to determine
  whether an MMIO access comes from proc0 or proc1. This is a QEMU guest CPU
  service, not the host machine CPU id.
- Proc1 FIFO IRQ output is routed to proc1. Peripheral IRQ routing beyond SIO
  is still mostly proc0-focused.

## Phase 14: Instantiate Cortex-M0+ Proc1

- [x] Split the current single `ARMv7MState armv7m` into proc0/proc1 state
  while preserving proc0 behavior.
- [x] Keep proc1 held in reset or dormant after machine reset.
- [x] Wire proc1 to the shared RP2040 memory map and `clk_sys`.
- [x] Add separate IRQ and NMI routing storage for proc0 and proc1.
- [x] Route `SIO_IRQ_PROC1` only to proc1.
- [x] Ensure proc0 boot, UART, flash, timer, watchdog, and boot ROM tests still
  pass unchanged.
- [ ] Document that dual-core scheduling is functional, not cycle-accurate.

## Phase 15: SDK-Compatible Core1 Launch

- [x] Model the reset/power path used by Pico SDK `multicore_reset_core1()`,
  including the `PSM_FRCE_OFF_PROC1` behavior or an equivalent documented
  minimal shim.
- [x] Support the Pico SDK FIFO launch sequence `{0, 0, 1, VTOR, SP, PC}` in
  the synthetic ROM, echoing the command words as the SDK expects.
- [x] Start proc1 with the provided vector table, stack pointer, and entry
  point.
- [x] Implement enough launch behavior for the SDK-style FIFO handshake to
  complete once core1 is already waiting in synthetic ROM.
- [x] Add a bare-metal functional test where proc0 sends the SDK launch
  sequence, receives echoes from proc1 through SIO FIFO, and then receives a
  post-jump `0xd01e` acknowledgement from code running on proc1's supplied
  stack.
- [x] Extend the synthetic ROM core1 path to jump to the provided `VTOR`,
  stack pointer and entry point after the sequence is validated.
- [ ] Add a Pico SDK multicore hello-world test once the SDK fixture is stable.
- [ ] Document remaining limitations: timing, lockout behavior, flash-write
  lockout interactions, divider/interpolator coverage, and reset fidelity.

## Phase 16: Documentation

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

## Phase 17: Patch Series Preparation

- [ ] Split the work into small reviewable commits.
- [ ] Keep SoC skeleton, machine, memory map, firmware loading, UART, tests, flash, persistence, and docs separate where practical.
- [ ] Ensure each commit builds independently.
- [ ] Run checkpatch on each commit.
- [ ] Run the relevant functional/qtest tests.
- [ ] Prepare the first submission as RFC if the model is still minimal.
- [ ] State clearly that this is not complete RP2040 emulation.

## Phase 18: Post-Integration Roadmap

- [ ] Add full SIO divider and interpolator datapaths.
- [ ] Improve timer fidelity.
- [x] Improve watchdog/reset behavior.
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
