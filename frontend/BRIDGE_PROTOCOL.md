# Bridge protocol (frontend ⇄ external device)

The frontend connects to the device over **TCP** (the device is the server) and talks
to it in **lock-step**: every access of the emulated 6809 to a bridged address blocks
the emulation until the device answers. The emulation is therefore deterministic and
the device sees the exact cycle of every access; the price is one network round trip
per access (≈ 0.1–1 ms on a LAN, so a bridged device makes the emulated machine run
slower than real time when it is accessed often). The frontend prints per-bridge
statistics with the `slots` command.

An ESP32 (or any Wi-Fi/Ethernet micro) can implement the device side directly. A
device with only a serial port needs a small TCP↔serial relay on the PC; the frame
format below is fixed-size, so a relay is just a byte pipe.

## Frames

All multi-byte values are little-endian.

**Request (emulator → device), 12 bytes**

| offset | size | field | meaning |
|-------:|-----:|-------|---------|
| 0 | 1 | `op` | ASCII, see below |
| 1 | 1 | `value` | data byte (writes, outputs) |
| 2 | 2 | `addr` | 6809 address, or the argument described for the op |
| 4 | 8 | `cycle` | 6809 cycles since start/reset, at the **start** of the instruction that made the access |

**Response (device → emulator), 2 bytes — always exactly one per request**

| offset | size | field | meaning |
|-------:|-----:|-------|---------|
| 0 | 1 | `data` | read data (ignored for other ops, send 0) |
| 1 | 1 | `flags` | bit 0 = IRQ asserted · bit 1 = PASS · bit 7 = control lines valid · bits 2–5 = CA1 CA2 CB1 CB2 input levels |

* **IRQ (bit 0)** is level-sensitive and carried by every answer. It drives the bus
  IRQ source given with `irq=N`; the CPU takes it when its I flag is clear. The device
  can only change it in an answer, so to raise an interrupt while the program is not
  touching the device, enable polling with `poll=CYCLES` (a `P` request is sent every
  that many cycles, e.g. `poll=20000` ≈ every 20 ms at 1 MHz).
* **PASS (bit 1)**: for a read, ignore `data` and let the emulator answer as if the
  device were not there; for a write, let the emulator also process the write.
* **Control lines (joystick bridge only)**: when bit 7 is set, bits 2–5 give the
  levels of the 6821 inputs CA1, CA2 (when an input), CB1, CB2. Changes are applied
  to the emulated PIA right after the current instruction and generate the usual
  edge/IRQ-flag behavior.

## Operations

| `op` | direction | used by | `addr` | `value` | answer `data` |
|------|-----------|---------|--------|---------|---------------|
| `R` read | CPU reads | expansion, cartridge | address | 0 | byte read |
| `W` write | CPU writes | expansion, cartridge | address | byte | ignored |
| `P` poll | every `poll=` cycles | all | 0 | 0 | ignored |
| `X` reset | machine reset | expansion, cartridge | 0 | 0 | ignored |
| `I` input pins | the CPU reads a PIA data register | joystick | port: 0 = A, 1 = B | 0 | pin levels (8 bits) |
| `O` port output | a PIA output or direction register was written | joystick | low byte = port (0 = A, 1 = B), high byte = DDR | output register | ignored |
| `C` control output | CA2 / CB2 changed while configured as an output | joystick | line: 1 = CA2, 3 = CB2 | level 0/1 | ignored |

For `O`, a bit is driven by the PIA when the same bit of the DDR is 1; the other bits
are inputs and must not be driven by the device.

If the device does not answer within 500 ms, or closes the connection, the bridge is
marked **DOWN**: the device is ignored from then on (reads fall back to the emulator,
the IRQ source is released) and a message is printed. Reconnecting requires restarting
the frontend.

## Command line

```
--bridge-exp  HOST:PORT@BASE+SIZE[,irq=N][,poll=CYCLES][,override]
--bridge-cart HOST:PORT@BASE+SIZE[,irq=N][,poll=CYCLES][,ctl]
--bridge-joy  HOST:PORT[,irq=N][,poll=CYCLES][,builtin][,std]
```

`BASE` and `SIZE` are hexadecimal. `irq=N` (1–31) wires the device IRQ to the CPU.
`override` lets the expansion window cover addresses the core already handles; `ctl`
also routes A7CB (cartridge control) to the device; `builtin` keeps the frontend's
joysticks active on the PIA pins (wired-AND with the device) and the DAC on port B
bits 0–5 driving the speaker; `std` uses the data-sheet register order instead of the
Thomson one. See `src/slots.h` for the address facts the slots rely on.

## Minimal device (pseudo-code)

```
loop:
  read 12 bytes -> op, value, addr, cycle
  switch op:
    'R': data = my_read(addr)
    'W': my_write(addr, value)
    'P', 'X': (nothing, or reset your state on 'X')
    'I': data = pins(port = addr)
    ...
  send [data, irq ? 1 : 0]
```

`tools/fake_micro.py` is a complete reference implementation (a 16-register device
with an IRQ, and a PIA-side device with a CA2→CA1 loopback) used by the tests.
