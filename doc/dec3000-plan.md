# DEC 3000 (EV4) emulation plan

Goal: run DEC OSF/1 V1.2 on an emulated DEC 3000 Model 400/500
("Sandpiper"/"Flamingo"), using the real console firmware and PALcode.
No other emulator models the 21064 (EV4) or the TURBOchannel Alphas.
(The original goal was V1.0, but it seems never to have been distributed
in binary form; V1.2 is the oldest release at hand, and it supports the
3000/400.)

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

- OSF/1 was written against DEC's console and PALcode; emulated
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

Everything below is in `media/` (not in git).

- Hardware: DEC 3000/400, /600, /700 (condition unknown). The /400 is the
  primary target (21064, supported by OSF/1 1.2). The /600 and /700 are
  21064A-based later models: useful for register dumps and their ROMs,
  probably not for OSF/1 1.2 itself.
- Manuals: `21064.pdf` (21064/21064A Hardware Reference Manual,
  EC-Q9ZUC-TE) and `EK-D3SYS-PM.B01.pdf` (DEC 3000 System Programmer's
  Manual).
- Firmware: `dec3000fw/` (V7.0 updaters for the 300, 400/600/700 and
  500/800/900) and two firmware CDs (V3.2 and V3.3 for all models, V4.0
  for the 500). Each updater (.EXE, .SYS, .BTP) carries the ROM images as
  plain copies; `experiments/dec3000_extract_rom.py` extracts them:
  SYSTEM_ROM, IO_ROM, and a second SYSTEM_ROM variant (flags byte 0x2a/
  0x2b is 01/00 for the first, 00/01 for the second; which one a /400
  uses is still open). The 64 KB serial ROM is not in the updaters (it's
  not field-updatable); it has to be emulated (see M4).
- OS media: OSF/1 V1.2, V1.3A, V2.0B (with layered products). **No V1.0
  media so far.** The CDs have a UFS file system (disk label at sector 0),
  not ISO 9660.
- Source: `OSF1-1.0-src.tar.bz2` and `osf1-src-v2.0.tgz` contain the same
  source kit (OSC200, DEC OSF/1 V2.0), including the Alpha kernel:
  `arch/alpha` (locore, trap handling, pmap, `softfp.c`), `arch/alpha/hal/
  kn15aa.c` (DEC 3000/400/500), `dec/sas/alpha` (boot programs),
  `io/dec/scsi/alpha` (TCDS/53C94 DMA). No PALcode source.

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
  Done: `experiments/dec3000_extract_rom.py`.
- Load it and start in PAL mode in the state the serial ROM leaves
  (SPM chapters 11 and 14). The serial ROM copies the system ROM into
  memory and jumps to it; the system ROM starts with an uncompressed PAL
  mode decompressor (at offset 0x40, after the header) that copies the
  I/O ROM and decompresses the console. From its code, the serial ROM
  puts the system ROM at 0x4000 and the I/O ROM at 0x44000, and starts at
  0x4040; the decompressed console runs at 0x54000.
- Flash/NVRAM environment variables.
- Work through the self-tests using the unmapped-access log;
  `show config`, `show device`, `test`.

### M5: NetBSD/alpha as a debuggable stepping stone
- Boot from an emulated disk via the real SRM: exercises interrupts,
  clock, SCSI DMA, LANCE, TC probing.

### M6: OSF/1 V1.2
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

- M2: 21064 privileged architecture in `cpu_alpha_ev4.c`, used when
  `ev4_palcode` is set (machines that run real PALcode; the others keep
  the emulated OSF/1 PALcode):
  - PALmode, all PALcode entry points, CALL_PAL dispatch (privileged and
    unprivileged ranges), EXC_ADDR with the PAL bit and the arithmetic
    trap/machine check bit 1 convention.
  - HW_MFPR/HW_MTPR with all Ibox and Abox IPRs and PAL_TEMPs (read
    formats as in the HRM), HW_LD/HW_ST (physical, ALT mode, read with
    write check, locked/conditional), HW_REI; ICCSR<HWE>.
  - ITB (8 + 4 large) and DTB (32, granularity hints), superpages
    (ICCSR<MAP>, ABOX_CTL<SPE_1>, <SPE_2>), D-stream faults with VA and
    MM_CSR (locking), ITB miss/ACV, bad VAs. The TBs are architectural
    state; gxemul's translation tables only cache them. Instruction fetches
    use tagged addresses (PALmode, native, data are separate).
  - Interrupts: six irq_h pins (`<cpu>.0` .. `<cpu>.5`), software
    interrupts, ASTs, HIRR/HIER/SIRR/SIER/ASTRR/ASTER; taken between
    instruction batches and right after HW_REI.
  - Machine checks for accesses to nonexistent physical addresses (BIU_STAT,
    BIU_ADDR), taken after the instruction; the cycle counter (one cycle
    per instruction).
  - FEN for floating point with ICCSR<FPE> clear.
  - Corrections to M1 from the HRM: the FPCR records all exceptions (not
    only trapping ones); divides never set FPCR<INE>, DIVx/SUI always
    trap with INE; IEEE rounding towards +/- infinity (/M, /D with DYN =
    1 or 3) and IEEE data type 1 raise FEN on the EV4 (the OS emulates
    them, see `softfp.c`); an underflow without /U gives +0 (confirmed by
    the HRM, section 2.7).
  - Tests: `test/alpha/pal_ev4.S` is a small PALcode of its own with 100
    checked results (machine `testalphapal`).
  - With the V3.3 /400 ROM loaded as the serial ROM would, the decompressor
    runs (~30M instructions) and the console starts at 0x54000 with its
    own PALcode; it stops at the first DEC 3000 I/O access (0x1_D4C0_0000
    etc.), which is M3.
  - Not done: performance counters, Icache/ASNs, cache and parity errors.
    Every mode change flushes gxemul's translation tables (simple and
    correct, but slow for an OS: to be optimized when OSF/1 runs).

- M3 (started): machine `dec3000 -e 400` (`machine_dec3000.c`,
  `dev_dec3000.c`): TURBOchannel dense/sparse decoder, TC ASIC CSRs
  (IOSLOT, TCCONFIG, FADR/TCEREG, MCRs, IMR/IR, S/G map), IOCTL registers,
  FEPROM, ESAR, SCCs and RTC (reused DECstation models), LANCE, empty
  slots time out with a machine check. Emulated serial ROM: copies the ROM
  contents *without* their 0x40-byte header to 0x4000 (system) and
  0x44000 (I/O) and starts at 0x4000 in PALmode. Takes an updater file or
  extracted images as the file to load.
  - Serial ROM hand-off (found from the console code; the PALcode reset
    entry at 0x5a0b4 saves the registers at 0x5a000):
    - `at` = memory banks, one byte per bank with the size code in bits
      <7:6> (1 = 8 MB, 2 = 32 MB, 3 = 128 MB); `a4` = memory size in MB.
    - `ra` = serial ROM version: ASCII digits in bytes 1 and 3, shown as
      "sVM.m" with M = byte1 - 4 and, only when M is 1, m = byte3 - 3
      (on the 400; other models have other offsets). Anything that isn't
      a digit gives "sV;.?". The real value is unknown; V2.0 is passed.
    - `t10` = memory error information: the number of its lowest set bit
      (mod 8) becomes the middle digit of the MEM status code when a
      memory test fails. 0 for good memory.
    - `a1`..`a3` are copied into the console's data (with a4 and ra);
      `t6`, `t7`, `s1`, `s2`, `a5` are saved too. Markers in them changed
      nothing visible in the power-up tests or `show config`; passed as 0.
    - Not the serial ROM's: +0x40, +0x70, +0x78 of the save area come from
      BIU_STAT<9>, PAL_TEMP18 and DC_STAT<2:0>.
    - The NVR test's "calculated" frequency is the console's own
      measurement against the RTC (expected: 133 MHz for system type 4,
      code 1), not a hand-off value: it depends on the emulator's speed
      (12-21 MHz, varying with host load).
  - A machine config file (`gxemul machine.json`, type "dec3000", model
    "400", "rom" = an updater or concatenated ROM images, "nvram" = a
    file) keeps the console's NVRAM between runs.
  - Status (V3.3): power-up tests run, the console reaches `>>>`, `show
    config`, `show device` and `show error` work. Self-test results:
    - ASIC: OK.
    - NVR: only "WRONG FREQUENCY, CHECK OSCILLATOR" (a warning): the
      console's calibration gives 12-18 MHz instead of 133. The cycle
      counter now runs on host time at the machine's clock rate, so the
      measurement must use something else (probably counted loop
      iterations against the RTC, i.e. the emulator's speed). Not
      understood yet.
    - MEM: "test LLSC addr = 00200000" fails (store conditionals now
      check the locked 32-byte block; not enough).
    - SCSI: the 53C94 register test fails on details of dev_asc (reads
      from an empty FIFO, Gross Error on FIFO overflow); fixes for those,
      plus interrupt-line tracking, a selection timeout delay, and bus
      reset interrupts not cleared by later commands, are in
      `experiments/dec3000_dev_asc_wip.patch`. With them, the test gets
      further but then loops in bus resets with 16 s delays: after a
      selection timeout the console expects its per-target state to be 9
      (code at 0x9d518 in the V3.3 console) and otherwise aborts (SETATN,
      bus reset). To be continued.
    - TCDS interrupt bits: OSF/1 (`pdmaflam.h`) uses CIR<19:18> for the
      53C94s, as the SPM says; the console's test also waits for CIR<20>
      with IMER<4> enabled. Both are set (CIR<21:20> only when IMER<5:4>
      enable them).
    - ISDN: a register-level 79C30A model is there, but the test's
      indirect register 0xa2 still reads back 0 (register layout or byte
      lane not right yet).
    - SCC: OK. IOCTL SCC DMA (comm port and printer port, transmit and
      receive, as on the 3MIN's IOASIC but with 34-bit pointers). Needed
      in dev_scc: a reset discards the receive FIFO, a disabled receiver
      gets nothing in loopback (the polled loopback test leaves a
      character behind otherwise), and an LK401 that answers commands
      even without a framebuffer (`dev_scc_kbd_present()`), as the
      console tests the keyboard line. Loopback output is no longer
      echoed to the terminal.
    - NI: OK. dev_le through the IOCTL's LANCE DMA pointer
      (`dev_le_dma_init()`, shared with the 3MIN), the same gap16/gap2
      layout. The console's tests need CSR1<0> to read back and 8-byte
      frames in loopback.
  - TODO: SCSI boot from disk/CD, FEPROM programming, ISDN DMA, the LANCE
    reset and DMA enable bits in SSR, the real serial ROM version.

## Things to get from the real machines (when they run)

- ROM dump of the /400 (and /600, /700 for comparison).
- `show config`, `show device`, `show *` output; power-up self-test log.
- TC option ROM dumps of any installed cards.
- Checks of CPU behaviour where the documentation is unclear:
  - AMASK on the 21064 (emulated: Rc = Rb).
  - The order of the register write mask bits read from EXC_SUM (the HRM
    says "F31-F0 then I31-I0"; emulated: F31 first).
  - Which of the two system ROM images in the updater a /400 uses.
  - (Answered by the HRM: an underflow without /U gives +0; the FPCR
    records all exceptions, trapping or not.)
