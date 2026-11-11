# MVME187 status

Goal: run the real 187Bug firmware and UNIX SYSTEM V/88 R4 (R40V4.4) on
an emulated MVME187. Reached: 187Bug runs, SYSTEM V/88 boots to
multi-user, root login on the console, the kernel can relink itself and
reboot the board, and the network works (ping, TCP through the simulated
gateway).

## Done

- 88100: complete SFU1 FPU, all exceptions delivered, missing integer
  instructions (add/sub with carry and overflow, tbnd, tcnd, fxcr, any
  bcnd mask, scaled byte accesses, xmem forms), MXM, rte reloading the
  pipeline from SNIP/SFIP, unimplemented opcodes trap.
- MC88200: all commands (probe, PATC invalidation), cache ports, bus
  errors with PFSR/PFAR; page tables may be outside RAM.
- Board (dev_mvme187.c): EPROM and ROM0 alias, SRAM, VMEchip2 (LCSR,
  GCSR, A16 self access, tick timers, interrupter, SYSRESET and GCSR RST),
  PCCchip2 (timers and prescaler, interrupt control, pin inputs, 88100
  prioritizer, SCC pseudo-IACK), MEMC040, IACK space, bus errors.
- CD2401: four channels, interrupt contexts, per-channel LIVR, loopback.
- 53C710: SCRIPTS processor and SCSI target model.
- MK48T08 with NVRAM file; a blank board configuration area
  ($FFFC1EF8, with its checksum at $FFFC1FF7) is filled in.
- 82596CA (dev_i82596.c): linear mode, big-endian layout, all commands,
  simplified and flexible receive and transmit, loopback; 187Bug's LANC
  diagnostics pass and SYSTEM V/88's e1x7 driver works.
- dyntrans: instruction pages under a device are not mapped to RAM.

## 187Bug self-test failures left

- RAM PED and MEMC RAMCNTRL: no parity error emulation (MEMC040 WWP).
- ST2401 DMA and BAUD: CD2401 DMA mode not implemented (time-outs).
- CMMU cache tests (CCHSD, CCHLR, CCHBYP, CCHSDWT, CCHBUR, ...): the
  cache contents are not emulated; CCHCODE ends the self-test with an
  unimplemented opcode exception.

## To do

- CD2401 DMA (if a driver uses it), modem signals, break.
- MCECC (ECC memory controller variant), a second MEMC040.
- WARNING: "pid 11(bcheckrc): trap type 3 return at spl7" on the first
  boot: look at the data access exception return path.
- Instruction timing: one instruction per bus clock is optimistic.
