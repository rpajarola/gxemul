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
- CD2401: four channels, interrupt contexts, per-channel LIVR, loopback,
  receive data timeout (data below the FIFO threshold is passed on after
  RTPR prescaler ticks; SYSTEM V/88's console needs it), asynchronous DMA
  (A/B buffers, special character exceptions, receive timeout, REOIR
  TermBuff/DiscExc), transmission paced at the baud rate.
- VMEchip2 watchdog (time-out, WDTO, BRDFAIL, reset).
- Emulated time: one instruction per bus clock, plus wait states for
  VMEchip2 registers (60 clocks, an estimate from 187Bug's poll budgets),
  BBRAM (a local bus cycle: 16 clocks plus 5 or 9 by the PCCchip2 FAST
  bit) and PCCchip2 registers (5 clocks). Devices can ask for an event at a given
  instruction count (machine_add_event); the dyntrans run then stops
  there, so tick timer and CD2401 interrupts come on time rather than at
  the next tick.
- Idle: spin loops which only read memory (such as SYSTEM V/88's idle
  loop, "tb1 1,r0,0xff" then loads, compares and branches) make the CPU
  idle; it skips ahead to the next tick or device event, and the host
  sleeps until its clock catches up (machine->realtime_hz). Busy code
  still runs as fast as it can, at most 0.1 s ahead of real time.
- 53C710: SCRIPTS processor (with the 53C710's register read/write
  operators) and SCSI target model; for the chip tests, the SCSI and DMA
  FIFOs (CTEST3/CTEST6, counts and parity), loopback of SODL/SOCL to
  SBDL/SBCL, CTEST5 ADCK/BBCK and CTEST8 CLF.
- DRAM parity: bytes written with the MEMC040's WWP bit get wrong parity;
  reading them with EXTPEN and PAREN is a bus error (VMEchip2 MLPE), with
  PARINT a VMEchip2 PE interrupt. A device on top of the RAM watches the
  accesses only while this matters. "tb0/tb1" (the barrier 187Bug uses
  after such reads) take a pending interrupt.
- MK48T08 with NVRAM file; a blank board configuration area
  ($FFFC1EF8, with its checksum at $FFFC1FF7) is filled in.
- 82596CA (dev_i82596.c): linear mode, big-endian layout, all commands,
  simplified and flexible receive and transmit, loopback; 187Bug's LANC
  diagnostics pass and SYSTEM V/88's e1x7 driver works.
- dyntrans: instruction pages under a device are not mapped to RAM.

## 187Bug self-test failures left

- CMMU cache tests (CCHSD, CCHUD, CCHSC, CCHUC, CCHSDWT, CCHUDWT,
  CCHBUR, CCHST, CCHWHIT, CCHLDS, CCHLR, CCHBYP): the cache contents
  are not emulated. "skip_failing_tests" patches them to pass.

## To do

- CD2401: synchronous modes, interrupt-mode transmit timing (characters
  go out at once), modem signals, break.
- MCECC (ECC memory controller variant), a second MEMC040.
- Instruction timing: one instruction per bus clock is optimistic. (A
  BBRAM access is charged a whole local bus cycle, estimated at 16 bus
  clocks plus the device's 5 or 9: SYSTEM V/88 calibrates the bus clock
  by polling the MK48T08 for the seconds to change, giving up after
  100000 polls, and fell back to 25 MHz when they were faster.)
