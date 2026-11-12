# DECpc AXP 150 / DEC 2000 Model 300 ("Jensen") Emulation Plan

**Goal:** Emulate the **DECpc AXP 150 / DEC 2000 Model 300** (code-named *"Jensen"*), Digital's first entry-level PC-architecture Alpha system. The machine uses the 64-bit Alpha 21064 (EV4) processor mated to an **EISA** (Extended Industry Standard Architecture) bus rather than TURBOchannel or PCI. It supports booting real SRM and ARC firmware to run **Microsoft Windows NT (3.1, 3.5, 3.51, 4.0) Alpha**, **OpenVMS AXP**, **DEC OSF/1**, and **Linux/Alpha**.

---

## 1. System Architecture Overview

```
                      +-----------------------------+
                      |     DECchip 21064 (EV4)     |
                      |          @ 150 MHz          |
                      +--------------+--------------+
                                     |
                       System Bus (64-bit, 150 MHz)
                                     |
                +--------------------+--------------------+
                |                                         |
     +----------+-----------+                   +---------+----------+
     | Main Memory & Bcache |                   |  Jensen EISA ASIC  |
     | Up to 128 MB SIMMs   |                   | Sparse I/O & Mem   |
     | 512 KB Write-Back L2 |                   | HAE Bank Register  |
     +----------------------+                   +---------+----------+
                                                          |
                                           EISA Bus (32-bit, 8.33 MHz)
                                                          |
    +---------------+---------------+---------------+-----+---------+---------------+
    |               |               |               |               |               |
+---+----+      +---+----+      +---+----+      +---+----+      +---+----+      +---+----+
| Slot 1 |      | Slot 2 |      | Slot 3 |      | Slot 4 |      | Slot 5 |      | Slot 6 |
| AHA-   |      | QVision|      | DE422  |      | Option |      | Option |      | Option |
| 1742A  |      | VGA    |      | NIC    |      |        |      |        |      |        |
+--------+      +--------+      +--------+      +--------+      +--------+      +--------+
    |
+---+-------------------------------------------------------------+
| Intel 82357 ISP (Integrated System Peripheral):                 |
|   - Dual 8259A cascaded PICs (IRQs 0..15 -> EV4 irq_h[3])       |
|   - 8254 PIT (Ports 0x40..0x43)                                 |
|   - Dual 8237 DMA Controllers (32-bit EISA DMA)                 |
|                                                                 |
| VL82C106 Combo Super I/O (Local Sparse Port 0x1C0000000):       |
|   - 8042 Keyboard & PS/2 Mouse (Ports 0x60, 0x64)              |
|   - DS1287 / MC146818 Real-Time Clock & NVRAM (Ports 0x70, 0x71)|
|   - Dual 16550A UARTs (COM1 0x3F8, COM2 0x2F8)                  |
|   - Parallel Port (LPT1 0x3BC)                                  |
|                                                                 |
| 1 MB Flash EPROM:                                               |
|   - Dual-personality ROM: SRM Console + ARC Console             |
+-----------------------------------------------------------------+
```

### Key Differences from DEC 3000/400
* **No TURBOchannel:** All expansion is standard 32-bit **EISA** slots (slots 1..6) and on-board motherboard peripherals.
* **Standard PC I/O:** Uses an 8042 PS/2 keyboard/mouse controller instead of LK201/VS423, standard 16550A UARTs instead of Z8530 SCC, and dual cascaded 8259 PICs instead of IOASIC/TCDS interrupt registers.
* **Sparse EISA Addressing:** The EV4 lacks byte/word loads and stores. All EISA I/O and memory accesses use Jensen's sparse address windows.
* **Adaptec SCSI:** The default SCSI controller is an **Adaptec AHA-1742A / 1740** EISA bus-master adapter rather than the NCR 53C94.

---

## 2. Memory & I/O Address Space

Jensen's physical address space (34-bit physical addressing on EV4), per
DEC EK-A0638-TD.001 and as implemented in `src/devices/dev_jensen.c`:

| Physical Address | Description |
| :--- | :--- |
| `0x0.0000.0000` | System RAM (up to 128 MB) |
| `0x1.0000.0000` | Interrupt Acknowledge (INTA) cycle register (82357 ISP) |
| `0x1.8000.0000` | Flash ROM window 0 (sparse, `addr >> 9`) |
| `0x1.A000.0000` | Flash ROM window 1 (sparse, `addr >> 9`) |
| `0x1.C000.0000` | Local Combo Super I/O (`VL82C106`: KBD, RTC, COM1, COM2, LPT) |
| `0x1.D000.0000` | Hardware Address Extension (HAE) register |
| `0x1.E000.0000` | System Control (SYSCTL) register (RAM size, watchdog, LEDs) |
| `0x1.F000.0000` | Spare register |
| `0x2.0000.0000` | Sparse EISA Memory Space (windowed via HAE) |
| `0x3.0000.0000` | Sparse EISA I/O Space (128-byte stride, width in bits 5..6) |

### Sparse I/O Address Decoding Formula
For an EISA I/O port `addr`:
* `physical_address = 0x300000000ULL | (addr << 7) | size_code`
  * `size_code = 0x00`: 8-bit byte access (`data & 0xFF`)
  * `size_code = 0x20`: 16-bit word access (`data & 0xFFFF`)
  * `size_code = 0x60`: 32-bit longword access (`data & 0xFFFFFFFF`)
* For local on-board Combo chips (keyboard, RTC, COM1, COM2, LPT1):
  * `physical_address = 0x1C0000000ULL | (addr << 9)`

---

## 3. Available Firmware Media

We have located and extracted genuine DEC firmware from `media/dec-osf1-2.0b_firmware-v2.8_cd.iso`:
* `media/jensenfw/JENSFW.BIN;1`: 1 MB full Flash ROM image containing `JENS_SYS` V1.1 (header width 1, stride 9, vendor `DEC`).
* `media/jensenfw/DEC2000_V1_3.EXE;1`: Executable firmware updater.
* `media/jensenfw/DEC2000_V13_FW_RELNOTE.TXT`: Official firmware documentation and release notes.

The firmware supports setting `os_type` to dynamically select console personalities:
* `set os_type nt`: Boots into the ARC (AlphaBIOS) console for Windows NT Alpha.
* `set os_type openvms`: Boots into the SRM console for OpenVMS AXP.
* `set os_type osf`: Boots into the SRM console for DEC OSF/1 / Digital UNIX / Linux.

---

## 4. Status (updated 2026-11-09)

Machine type is `jensen` (model `300`), e.g. `gxemul -e jensen -x -M 64 media/rom/jensen_v1_3.rom`.
User-facing documentation lives in `doc/machine_jensen.html`.

### Done

- **Milestone 0 &ndash; ROM extraction:** `media/rom/jensen_v1_3.rom` (1 MB) extracted from `JENSFW.BIN;1`.
- **Milestone 1 &ndash; Machine & bus router:** `src/machines/machine_jensen.c`, `src/devices/dev_jensen.c`.
  INTA, Flash windows (write-protected except NVRAM blocks), Combo I/O, HAE, SYSCTL,
  sparse EISA memory/I/O, dense EISA memory. Build and `machine.json` / config UI wired up.
- **Milestone 2 &ndash; PC peripherals:** cascaded 8259s (with ELCR1/ELCR2 level-trigger registers),
  8254 PIT, 82357 DMA incl. EISA extension registers, 16550 COM1/COM2, LPT1, 8042 KBC
  (+ port 0x61), MC146818/DS1287 RTC + CMOS (reg 0x4f = OS type), ISA VGA.
- **Milestone 3 &ndash; Firmware:**
  - SRM V1.3 passes all power-up tests and reaches `>>>` (`-o vms` / `-o osf`).
  - ARC Multiboot 3.0.g reaches the boot menu with no NVRAM warning (`populate_ecu`
    pre-populates NVRAM block 6 = ECU table and block C = ARC config tree, environment
    and EISA slot data; see `src/devices/dev_jensen_nvram.h`).
  - Serial console 7-bit VT100 translation for ARC's 8-bit C1 output (`-7` / `"vt100": true`).
- **Milestone 4 &ndash; EISA & SCSI (partial):**
  - EISA IDs: system board `DEC2400` at 0xC80, AHA-1742A (`ADP0001`) in slot 1.
  - `src/devices/dev_aha1742.c`: enhanced mode, mailboxes, ECBs, scatter-gather bus-master DMA, IRQ 14 (from the ECU configuration).
  - Intel 82077AA floppy controller (`src/devices/dev_fdc.c`), PIO and DMA channel 2;
    ECU V1.9 runs from a diskette image and saves its configuration.
- **Milestone 5 &ndash; OS verification (partial):**
  - OpenVMS AXP V1.5-1H1 boots from CD via `SYS$PKJDRIVER` to Stand-alone BACKUP (`$`).
  - Windows NT 3.1 Alpha (build 528) installs from scratch (arcinst, ECU from
    ARC, text-mode and GUI-mode Setup) and boots to the logon screen and
    Program Manager on the QVision; see "Installing Windows NT 3.1 from
    scratch" in `doc/machine_jensen.html`.

### NT 3.1 text-mode Setup (2026-11-09)

Text-mode Setup runs to 100%: arcinst creates and formats the 5 MB FAT system
partition, Setup creates/formats D: (FAT), and copies all files (152 files,
19.6 MB in `\winnt\system32`, plus HAL/PALcode/OSLOADER on the system partition).
Fixed on the way:

- AHA-1742A: SCSI writes were silently dropped (`data_out_offset` never set);
  bus-master DMA could cross a memblock boundary and `exit(1)`; completions are
  now queued when the host has not yet cleared the previous interrupt.
- 8259 (Jensen only): a request that arrives while the same IRQ is in service
  survives the EOI; level-triggered inputs (ELCR) stay requested while active.
- Flash NVRAM persistence: with an `nvram` file set, the whole Flash (ARC
  environment, config tree, ECU data, boot selections) is kept in `<nvram>.flash`.

### Next

1. **Windows NT 3.1 installs and runs** on the emulated QVision (Setup to
   "Windows NT 3.1 is now installed", then logon with Ctrl+Alt+Del, which
   the 8042 now types with its modifiers, and Program Manager). QV.SYS/QV.DLL draw with the packed 8-bit mode, the
   linear framebuffer and the BitBLT engine (worked out from what QV.DLL
   writes). Open:
   - Now and then (about 1 boot in 15 since the lock flag is cleared on
     interrupts; 3 in 8 before), GUI Setup stops after the desktop fill: a
     lock-order deadlock in csrss. The API thread cleaning up a dead client
     thread (`CsrDestroyThread`) holds CSRSRV's process structure lock
     (0x77d2a0f8) and waits for WINSRV's USER lock (0x75c848b8); a thread
     serving a USER/GDI request holds that and waits for the CSRSRV lock.
     winlogon then waits in `CsrClientSendMessage` forever. The dead client
     thread is a winlogon thread that exits with `C000004B` just as the
     GUI starts (also in good boots). Not known whether that exit is
     normal, or why our timing hits the window.
   - No mouse pointer: NT never sets a pointer shape (mouse not detected?).
   - The year shows as 2006 (item 3).
   - arcinst must run before the ECU: with the QVision's framebuffer
     reserved at 4 MB, arcinst (at 6 MB) fails with "Failed to initialize
     the heap".
   Earlier blockers, fixed: csrss died at once (EV4 reported the aligned
   address for STQ_U faults, which ntdll's IAT-forwarder filter needs
   exactly), and the RTC ran NT's clock 128 times too fast (the Jensen's RTC
   always runs from 32.768 kHz). The QVision must be configured by the NT
   ECU from ARC (`CPQ3011` display node, HMA at 4 MB).
2. **ARC system ID is `DEC-00Jensen`**; NT Setup only knows `DEC-10Jensen` /
   `DEC-20Jensen` (Computer: Unknown, must be picked by hand). The digit comes from
   word 1 of the parameter block the ROM loader passes to the ARC entry point
   (`{1, 0, 6600 ps, ...}`); it is not SROM register T11. Find its source.
3. **Year shows as 2006** in the ARC clock (RTC year convention).
4. **Video:** NT 3.1 Alpha only supports Compaq QVision and S3 (911/924, 928)
   displays; there is no plain VGA option. The QVision works (item 1).
5. DEC OSF/1 AXP; DE422 NIC; the mouse; newer NT/VMS.

### Not emulated

- PC speaker tone (8254 counter 2) &ndash; logged at debug level only.
- 8254 counter 1 (DMA refresh) &ndash; accepted, nothing is done with it.
- 8254 counts do not run down: a counter reads back the value loaded.
