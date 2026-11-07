# GXemul Planned Features & TODO

This document tracks planned features, enhancements, and known open items for this fork of GXemul.

The primary focus of this fork is accurate, high-performance, and usable emulation of **DECstation** (MIPS) and **DEC 3000 AXP** (Alpha) systems, running real firmware and operating systems (Ultrix, DEC OSF/1, OpenVMS, NetBSD, OpenBSD, Sprite, Linux, and Windows NT), alongside modern host conveniences (SDL2, Windows/macOS/Linux packaging, MCP agent support, and robust machine configurations).

---

## 1. DEC 3000 AXP (Alpha EV4)

- [ ] **Mouse input in X11 / DECwindows Motif:** Complete mouse event handling through the SCC / LK201 for graphical sessions under DEC OSF/1 and OpenVMS.
- [ ] **TURBOchannel option ROM self-test compliance:** Ensure the HX (PMAG-RO / SFB) option ROM passes all power-up diagnostics cleanly without requiring workarounds (`TC0 ?? 300`).
- [ ] **Serial ROM (SROM) hand-off:** Verify and match exact real-hardware SROM version encoding passed to console firmware on Model 400/500/600/700.
- [ ] **Console calibration & frequency warning:** Address the firmware power-up warning `WRONG FREQUENCY, CHECK OSCILLATOR` where SRM calibrates CPU clock against RTC loops.
- [ ] **Real hardware dumps and verification:**
  - ROM dumps from physical DEC 3000/400, /600, /700 systems for comparison.
  - Verify 21064 CPU quirks where documentation is ambiguous: `AMASK` behavior, `EXC_SUM` register write mask bit ordering (F31-F0 vs I31-I0), and system ROM image variants.
- [ ] **Memory test LL/SC:** Address the `test LLSC addr = 00200000` power-up diagnostic failure.
- [ ] **Dyntrans PALmode switch optimization:** Optimize translation table flushing on PALmode / privileged mode switches to improve OS execution speed.
- [ ] **Additional DEC 3000 peripherals:**
  - FEPROM programming support.
  - ISDN DMA controller.
  - LANCE reset and DMA enable control bits in SSR.
  - Support for additional TURBOchannel option cards.

---

## 2. DECstation (MIPS) & Framebuffers

- [ ] **TURBOchannel framebuffer options:**
  - Emulate additional TURBOchannel cards (e.g. PMAG-JA, PMAG-RO / SFB on DECstations).
  - Add 2D acceleration features where supported by firmware and guest OS drivers.
- [ ] **Multi-head display ergonomics:** Improve UX when running multiple framebuffer displays simultaneously (window positioning, unified input focus).
- [ ] **FPU exception correctness:** Verify IEEE floating-point exception handling and edge-case flags against physical R3000/R3010 and R4400 hardware.
- [ ] **Count register & timer precision:** Fine-tune MIPS Count/Compare timer interrupt pacing under dynamic translation to prevent clock drift in guest OSes.
- [ ] **DECstation audio:** Emulate the IOASIC ISDN / AMD79C30 audio codec for sound output in Ultrix and NetBSD.
- [ ] **Sprite multi-node networking:** Investigate root filesystem mounting (`/`) when running multiple networked Sprite DECstation nodes simultaneously.

---

## 3. Core Emulation & Dynamic Translation (Dyntrans)

- [ ] **SMP translation invalidation:** Replace coarse machine-wide translation cache flushes with fine-grained per-page invalidation across CPUs when write-protected code pages are modified.
- [ ] **Graceful CPU stop vs exit(1):** Continue converting legacy `exit(1)` error paths into `cpu->running = false` transitions with proper dyntrans abort, allowing debugger inspection.
- [ ] **Instruction combination profiles:** Profile instruction patterns on modern 64-bit hosts for hot inner loops (`memcpy`, `memset`, string operations, idle loops) to generate optimized combined instructions.
- [ ] **Integer & boolean type cleanup:** Modernize legacy `int` flags to standard `<stdbool.h>` `bool` across CPU and device state structures.

---

## 4. Debugger, MCP & Diagnostics

- [ ] **Subsystem breakpoints:**
  - `breakpoint subsystem <subsys> <level>` to automatically break into the debugger on specific warning/error messages.
  - Clean breakout from the dyntrans execution loop when subsystem breakpoints fire.
- [ ] **Breakpoint hit counts:**
  - Support triggering breakpoints only after $N$ hits (`hits_until_break`), or counting hits without stopping (`hitcount`).
  - Display hit counts in `CTRL-T` status and `show breakpoints`.
- [ ] **CLI debugger usability:**
  - Symbol tab-completion and fuzzy matching for symbols in debugger commands.
  - Support spaces in arithmetic expressions (e.g. `u pc + 0x40`).
  - Add `find` command to search memory ranges for byte sequences or strings.
- [ ] **MCP server extensions:** Expand Model Context Protocol tools for automated introspection, memory inspection, and interactive test harnesses.

---

## 5. Networking & I/O

- [ ] **Network performance:**
  - Investigate throughput bottlenecks in simulated NAT / SLIRP mode for guest downloads.
  - Optimize LANCE Ethernet interrupt delivery and DMA ring handling.
- [ ] **Host audio backend:** Add SDL2 audio backend support when sound chip emulation (AMD79C30 / DECstation, Alpha ISDN) is implemented.
- [ ] **Web-based remote console & display streaming:** Expand beyond the existing VNC server (`--vnc`) and control socket (`--control`) to provide a zero-install browser interface for multi-machine setups.

---

## 6. Automated Testing & Verification

- [ ] **Guest OS CI regression testing:**
  - Automated headless test scripts booting guest OSes (NetBSD, Ultrix, OSF/1, OpenVMS) in CI to verify installer, bootloader, and multi-user milestones.
  - Automated detection of regressions in SCSI, LANCE, RTC, and console I/O.
- [ ] **CPU architecture conformance suites:** Expand standalone unit tests (`test/`) for instruction corner cases, TLB translations, and exception vectors.
