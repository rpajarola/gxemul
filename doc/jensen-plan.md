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

Jensen's physical address space (34-bit physical addressing on EV4):

| Physical Address | Size | Description |
| :--- | :--- | :--- |
| `0x0.0000.0000` &ndash; `0x0.07FF.FFFF` | 128 MB | System RAM |
| `0x1.0000.0000` &ndash; `0x1.000F.FFFF` | 1 MB | Flash EPROM (SRM / ARC Console Firmware) |
| `0x1.C000.0000` &ndash; `0x1.DFFF.FFFF` | 512 MB | Local Motherboard Combo I/O (`VL82C106`) |
| `0x2.0000.0000` &ndash; `0x2.FFFF.FFFF` | 4 GB | Sparse EISA Memory Space (windowed via HAE) |
| `0x3.0000.0000` &ndash; `0x3.FFFF.FFFF` | 4 GB | Sparse EISA I/O Space (128-byte stride) |
| `0x3.1000.0000` | 8 bytes | Interrupt Acknowledge (IACK) cycle register |
| `0x3.1800.0000` | 8 bytes | Hardware Address Extension (HAE) register |

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

## 4. Implementation Roadmap & Milestones

### Milestone 0: Branch & ROM Extraction
- Create feature branch `jensen`.
- Write Python script to strip the 81-byte text header from `JENSFW.BIN;1` into `media/rom/jensen_v1_3.rom` (1,048,576 bytes).
- Verify ROM header format and initial PALcode entry vector.

### Milestone 1: Machine Setup & Sparse Memory Router
- Create `src/machines/machine_jensen.c` and register machine type `dec2000` (subtypes: `300` for Model 300 / AXP 150).
- Create `src/devices/dev_jensen.c`:
  - Implement memory router for `0x1.0000.0000` (Flash ROM), `0x1.C000.0000` (Local Combo I/O), `0x3.0000.0000` (Sparse EISA I/O), and `0x2.0000.0000` (Sparse EISA Memory).
  - Implement `HAE` register and `IACK` cycle handling.
- Wire build files: `Makefile.skel`, `configure`, `automachine`.

### Milestone 2: Standard PC Peripherals Wiring
- Attach `dev_8259` dual cascaded PIC:
  - Route PIC interrupt request line to Alpha EV4 `irq_h[3]`.
  - Handle IACK cycle to return the interrupting vector to the PALcode dispatcher.
- Attach `dev_8253` PIT at ports `0x40..0x43`.
- Attach `dev_ns16550` UARTs: COM1 (`0x3F8`, IRQ 4) and COM2 (`0x2F8`, IRQ 3).
- Attach `dev_pckbc` (8042 keyboard/mouse) at ports `0x60`, `0x64` (IRQ 1 for keyboard, IRQ 12 for mouse).
- Attach `dev_mc146818` (DS1287 RTC) at ports `0x70`, `0x71` (IRQ 8).
- Attach `dev_vga` for video output (VGA registers at `0x3C0..0x3DF`, VRAM at `0xA0000`).

### Milestone 3: Firmware Boot to Prompt (SRM & ARC)
- Boot `jensen_v1_3.rom` on EV4 under GXemul.
- Verify firmware power-on self-tests:
  - Memory sizing & parity check.
  - 8254 timer & 8259 interrupt controller check.
  - 8042 keyboard controller check.
  - DS1287 RTC clock check.
- Reach interactive prompt:
  - SRM console: `>>>`
  - ARC console: Firmware menu (`Boot Windows NT`, `Run a Program`, `Set Up the System`).

### Milestone 4: EISA Bus & Adaptec AHA-1742A SCSI Controller
- Implement EISA slot product ID registers (`0xzC80..0xzC83` for slots 1..6):
  - Slot 1: Adaptec AHA-1742A (`ADP0001` or `ADP0002`).
  - Slot 2: Compaq QVision VGA or S3 (`CPQ0001`).
  - Slot 3: DEC EtherWORKS DE422 EISA NIC (`DEC4220`).
- Create `src/devices/dev_aha1742.c`:
  - Implement AHA-1542 standard compatibility mode (Control/Status/Command/Data registers).
  - Implement AHA-1740 Enhanced Mode 32-bit command rings (Mailbox Out, Mailbox In, EISA DMA).
  - Connect to GXemul's SCSI command engine (`diskimage_scsicmd.c`) for disk (`DKA0:`) and CD-ROM (`DKA400:`).

### Milestone 5: Operating System Verification
- Boot and install **Windows NT 3.51 / 4.0 Alpha** via the ARC console.
- Boot and install **OpenVMS AXP 1.0 / 1.5** via the SRM console (`boot dka400`).
- Boot **DEC OSF/1 AXP** and **Linux/Alpha**.
- Document procedures in `doc/machine_jensen.html`.
