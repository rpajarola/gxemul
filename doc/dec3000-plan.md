# DEC 3000 (EV4) emulation plan

Goal: run DEC OSF/1 V1.0 on an emulated DEC 3000 Model 400/500
("Sandpiper"/"Flamingo"), using the real console firmware and PALcode.
No other emulator models the 21064 (EV4) or the TURBOchannel Alphas, and
OSF/1 1.0 runs on nothing newer.

## Starting point (as of branch creation)

- Alpha CPU: decoder, dyntrans translator and disassembler exist. Integer
  coverage is reasonable; FP is only a handful of T-format ops. There is no
  exception delivery (`irq_asserted` is set but never checked), no PAL mode,
  no `hw_*` instructions, no IPRs, no TLB, no machine checks.
- `cpu_alpha_palcode.c` emulates ~15 OSF PALcode calls in C (with bugs, e.g.
  `swpctx` stores the flags into `ptbr`). `memory_alpha.c` is a page-table
  walk with hard-coded hacks that `abort()`s on a miss.
- `machine_alpha.c` has a stub "3000/300" with one z8530 at a made-up
  address and a minimal HWRPB.
- No Alpha ECOFF in the file loader (OSF/1 `vmunix` is ECOFF).
- Reusable from the DECstation side: `dev_scc.c` (Z85C30), `lk201.c`,
  `dev_le.c` (LANCE + IOASIC DMA), `dev_asc.c` (53C94), `dev_mc146818.c`,
  `dev_dec_ioasic.c`, `dev_turbochannel.c`, TC framebuffers (cfb, sfb/HX,
  mfb, px).

## Approach

Run the real SRM console ROM with its own PALcode on an accurate EV4.

- OSF/1 1.0 was written against DEC's console and PALcode; emulated
  PALcode is an endless tail of exception-frame, TB-miss, `swppal` and
  machine-check logout differences.
- Disk boot (boot block -> `osfboot` -> `/vmunix`) and CD installation go
  through console callbacks; the real firmware provides them.
- The EV4 privileged architecture has to be right either way.
- Risk: firmware self-tests (Bcache, memory, ASIC, LANCE/SCC loopback,
  SCSI). Implement loopback modes, or patch the tests
  (cf. `experiments/patch_rom_tests.py`).

Fallback / development harness: fixed-up high-level emulation (correct
DEC 3000 HWRPB, OSF PALcode in C, direct kernel load) for testing the CPU
before the firmware runs.

## Available material

- Hardware: DEC 3000/400, /600, /700 (condition unknown). The /400 is the
  primary target (21064, supported by OSF/1 1.0). The /600 and /700 are
  21064A-based later models: useful for register dumps and their ROMs,
  probably not for OSF/1 1.0 itself.
- Firmware: update images only (netbootable executable, bootable floppy).
  The raw ROM image has to be extracted from them, or dumped from the /400.
- OSF/1 V1.0 media, possibly source code.

## Milestones

### M0: test setup
- Instruction-level tests against an Alpha cross assembler
  (binutils-alpha-linux-gnu), run in gxemul's bare-machine mode.
  Reference results from the Alpha Architecture Reference Manual and, for
  user-mode semantics, QEMU alpha-linux-user (EV4-legal subset only).
- Tracing: PAL-mode entry/exit, IPR reads/writes, accesses to unmapped
  physical addresses (with PC), machine checks.
- Alpha ECOFF in the file loader.

### M1: complete EV4 user ISA
- All integer ops (`umulh`, `/v` forms, full byte manipulation,
  `ldx_l`/`stx_c`).
- BWX, CIX, FIX, MVI -> OPCDEC (EV4 has none of them). IMPLVER = 0,
  AMASK = 0.
- Full IEEE S/T FP: rounding modes, trap qualifiers, FPCR, arithmetic traps
  with EXC_SUM. VAX F/G/D at lower priority.
- Unaligned-access traps.

### M2: EV4 privileged architecture (21064 Hardware Reference Manual)
- PAL mode (PC<0>), PAL_BASE-relative entry points (reset, mchk, arith,
  interrupt, D-stream fault, ITB/DTB miss native/PAL, unaligned, OPCDEC,
  FEN, CALL_PAL privileged/unprivileged).
- `hw_mfpr`, `hw_mtpr`, `hw_ld`/`hw_st` (physical, alt-mode, lock),
  `hw_rei`.
- Ibox IPRs: TB_TAG, ITB_PTE, ICCSR, EXC_ADDR, EXC_SUM, PS, PAL_BASE,
  HIRR/HIER, SIRR/SIER, ASTRR/ASTER, ITB invalidates, serial line.
  Abox IPRs: DTB_CTL, DTB_PTE, MMCSR, VA, ABOX_CTL, ALT_MODE, CC/CC_CTL,
  BIU_CTL/STAT/ADDR, DC_STAT, FILL_*, BC_TAG, FLUSH_IC, PAL_TEMP[0..31].
- Software TLB: ITB 8 + 4 large-page entries, DTB 32 entries, granularity
  hints, ASM (no ASNs on EV4). The TB is architectural state; gxemul's VPH
  tables are only a cache of it, invalidated on TBIS/TBZAP/TBASM and on
  replacement. PAL mode (physical I-stream) gets a separate translation
  context.
- Interrupts: 6 hardware IRQ lines into HIRR, software interrupts, ASTs,
  cycle-counter interrupts; `rpcc` tied to `emulated_hz`.
- Machine checks on accesses to nothing, with logout frames (both console
  and kernel probe TC slots this way).
- Remove the C PALcode and the translation hacks, or move them behind the
  fallback option.

### M3: DEC 3000 platform (`machine_dec3000.c`)
- References: NetBSD `sys/arch/alpha` (`dec_3000_500.c`,
  `tc_3000_500.c`, `tcasic.c`, `tc/ioasic.c`, `tc/tcds*.c`,
  `tc_dma_3000_500.c`), DEC 3000 system programmer's/technical manuals,
  register dumps from the real machines.
- Physical memory map: RAM, TC slots (built-ins in slots 6/7), system
  ROM/flash, memory controller and Bcache CSRs.
- TC ASIC: interrupt mask/status, error registers, routing onto EV4 IRQ
  lines, TC DMA scatter-gather map.
- IOCTL ASIC (refactor `dev_dec_ioasic.c` to take a per-platform register
  layout, interrupt bits and DMA pointer format): 2x SCC (serial,
  LK201/mouse), LANCE, MC146818/DS1287, AMD 79C30 ISDN (stub).
- `dev_tcds.c`: dual 53C94/53CF94 SCSI + DMA, reusing `dev_asc.c` (FAST
  SCSI CFG3; CD-ROM for installation).
- Serial console first; framebuffers (built-in on the /500, TC options)
  afterwards.

### M4: real firmware reaches `>>>`
- Extract the ROM image from the update kits (or use the /400 dump).
- Load it and start in PAL mode in the state the serial ROM leaves.
  Open question: does the SRM run from ROM space or copy itself to RAM?
- Flash/NVRAM environment variables.
- Work through the self-tests using the unmapped-access log;
  `show config`, `show device`, `test`.

### M5: NetBSD/alpha as a debuggable stepping stone
- Boot from an emulated disk via the real SRM: exercises interrupts,
  clock, SCSI DMA, LANCE, TC probing.

### M6: OSF/1 V1.0
- Boot the install CD image, install to disk, boot multi-user.
- Expected trouble: HWRPB fields, VMS->OSF `swppal`, clock calibration.
- Networking, framebuffer + X last.

## Progress

- M0: `testalpha`/`barealpha` machines; instruction tests in `test/alpha`
  (see its README), with reference output from QEMU's EV4 model plus an
  exact floating-point model (QEMU's VAX floating point is wrong).
- M1: user-mode ISA complete except for trap delivery, which waits for M2:
  all integer operates incl. `/V`, BWX/FIX/CIX/MVI as reserved opcodes on
  EV4, soft-float IEEE S/T and VAX F/G/D with all rounding modes and the
  EV4 trap rules, FP branches, `rc`/`rs`, unaligned accesses fault.
  Exceptions currently stop the emulator with a message
  (`alpha_exception()`).

## Things to get from the real machines (when they run)

- ROM dump of the /400 (and /600, /700 for comparison).
- `show config`, `show device`, `show *` output; power-up self-test log.
- TC option ROM dumps of any installed cards.
- Checks of CPU behaviour where the documentation is unclear:
  - sign of an IEEE result that underflows without /U (emulated: +0,
    QEMU: signed zero);
  - which FPCR bits the 21064 implements and whether it sets the exception
    bits for non-trapping exceptions (emulated: only SUM, DYN and the
    exception bits exist, set only when a trap is taken);
  - AMASK on the 21064 (emulated: Rc = Rb).
