# MVME187 status

Goal: run the real 187Bug firmware and UNIX SYSTEM V/88 R4 (R40V4.4) on
an emulated MVME187. Reached: 187Bug runs, SYSTEM V/88 boots to
multi-user, root login on the console, the kernel can relink itself and
reboot the board.

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
- MK48T08 with NVRAM file.
- dyntrans: instruction pages under a device are not mapped to RAM.

## 187Bug self-test failures left

- RAM PED and MEMC RAMCNTRL: no parity error emulation (MEMC040 WWP).
- PCC2 ADJ: prescaler adjust test.
- ST2401 DMA: CD2401 DMA mode not implemented (the test hangs).

## To do

- 82596CA Ethernet (the kernel's e1x7 driver; slink fails without it).
- CD2401 DMA (if a driver uses it), modem signals, break.
- MCECC (ECC memory controller variant), a second MEMC040.
- WARNING: "pid 11(bcheckrc): trap type 3 return at spl7" on the first
  boot: look at the data access exception return path.
- Instruction timing: one instruction per bus clock is optimistic.
