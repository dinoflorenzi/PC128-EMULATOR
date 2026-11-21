#!/usr/bin/env python3
"""End-to-end tests of the frontend bridge: starts tools/fake_micro.py, runs the
headless frontend with a debugger script and checks both the emulator state it
prints and the requests the device received.

  test_bridge.py [path/to/theodore-fe] [path/to/theodore_libretro.so]
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "frontend", "theodore-fe")
CORE = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "theodore_libretro.so")
MICRO = os.path.join(HERE, "fake_micro.py")
ENV = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")

fails = 0


def check(cond, msg):
    global fails
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails += 1


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run(kind, bridge_opt, script, micro_args=(), timeout=120):
    """Returns (frontend output, device log entries, micro process)."""
    port = free_port()
    tmp = tempfile.mkdtemp()
    log = os.path.join(tmp, "ops.jsonl")
    sp = os.path.join(tmp, "script.txt")
    open(sp, "w").write(script)
    micro = subprocess.Popen([sys.executable, MICRO, kind, "--port", str(port), "--log", log] + list(micro_args),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    micro.stdout.readline()  # "listening ..."
    # bridge option is "--bridge-xxx SPEC": split it
    opt = bridge_opt.replace("PORT", str(port)).split(" ", 1)
    cmd = [FE, "--core", CORE, "--no-console", "--turbo", "--script", sp] + opt
    out = subprocess.run(cmd, env=ENV, capture_output=True, text=True, timeout=timeout)
    micro.terminate()
    try:
        micro.wait(5)
    except subprocess.TimeoutExpired:
        micro.kill()
    entries = []
    if os.path.exists(log):
        entries = [json.loads(l) for l in open(log) if l.strip()]
    return out.stdout + out.stderr, entries


def grab(text, pattern, default=None):
    m = re.search(pattern, text)
    return m.group(1) if m else default


# ---------------------------------------------------------------- 1. expansion
print("[1] expansion bridge: round trip, cycle stamps, IRQ")
script = """wait 200
halt
poke 3000 86 5A B7 A7 F2 86 A5 B7 A7 F3 B6 A7 F2 B7 40 00 F6 A7 F3 F7 40 01 86 01 B7 A7 FE 20 FE
set PC 3000
set CC 10
trace clear
trace on
bp pc 301B
go
waitstop 200
echo MEM
mem 4000 2
echo IRQ-ASSERTED
irq
echo TRACE
trace dump 12
echo --- release the IRQ by reading register 14
poke 3020 B6 A7 FE 20 FE
set PC 3020
bpclear
bp pc 3023
go
waitstop 200
echo IRQ-RELEASED
irq
slots
quit
"""
out, log = run("exp", "--bridge-exp 127.0.0.1:PORT@A7F0+10,irq=3", script)
check("bridge exp: 127.0.0.1" in out and "attached (IRQ wired)" in out, "bridge attached with IRQ wired")
check(re.search(r"MEM\n4000: 5A A5", out) is not None, "values read back through the device: 5A A5 stored at 4000")
check("IRQ-ASSERTED\nIRQ source mask: 00000008" in out, "writing register 14 asserted bus IRQ source 3")
check("IRQ-RELEASED\nIRQ source mask: 00000000" in out, "reading register 14 released it")
ops = [(e["op"], e["addr"], e["value"]) for e in log if e["op"] in "RW"]
check(("W", 0xA7F2, 0x5A) in ops and ("W", 0xA7F3, 0xA5) in ops and ("R", 0xA7F2, 0) in ops, "device saw W A7F2=5A, W A7F3=A5, R A7F2")
# cycle stamps: the emulator trace gives the start cycle of each instruction
trace = {}
for m in re.finditer(r"^\s*(\d+)\s+A=.*?\s([0-9A-F]{4}) [0-9A-F]+\s+(\w+)\s+(\S+)", out, re.M):
    trace[(int(m.group(2), 16))] = int(m.group(1))
w = [e for e in log if e["op"] == "W" and e["addr"] == 0xA7F2]
check(bool(w) and trace.get(0x3002) == w[0]["cycle"], "cycle stamp of STA $A7F2 matches the emulator trace (%s)" % (w[0]["cycle"] if w else "-"))
r = [e for e in log if e["op"] == "R" and e["addr"] == 0xA7F2]
check(bool(r) and trace.get(0x300A) == r[0]["cycle"], "cycle stamp of LDA $A7F2 matches the emulator trace (%s)" % (r[0]["cycle"] if r else "-"))
cyc = [e["cycle"] for e in log]
check(cyc == sorted(cyc), "device cycle stamps never go backwards")
check(re.search(r"expansion\s+127\.0\.0\.1:\d+ up, \d+ transactions", out) is not None, "`slots` reports the link as up with statistics")

# ---------------------------------------------------------------- 2. joystick / PIA
print("[2] joystick bridge: PIA pins, port outputs, CA2->CA1 loopback IRQ")
script = """wait 200
halt
poke 3000 86 00 B7 A7 CE 86 F0 B7 A7 CC 86 04 B7 A7 CE 86 A5 B7 A7 CC B6 A7 CC B7 40 00 86 37 B7 A7 CE 86 3F B7 A7 CE 20 FE
set PC 3000
set CC 10
bp pc 3024
go
waitstop 300
echo MEM
mem 4000 1
echo PIA-IRQ
pia
irq
echo --- a data read clears the flag and releases the IRQ
poke 3030 B6 A7 CC 20 FE
set PC 3030
bpclear
bp pc 3033
go
waitstop 300
echo PIA-CLEAR
pia
irq
quit
"""
out, log = run("joy", "--bridge-joy 127.0.0.1:PORT,irq=4", script, ["--pins-a", "3c"])
check("bridge joy: 127.0.0.1" in out, "joystick bridge attached")
check(re.search(r"MEM\n4000: AC ", out) is not None, "port A read = (pins 3C & ~DDR F0) | (ORA A5 & DDR F0) = AC")
check(("O", 0x00F0 | 0, 0x00) in [(e["op"], e["addr"], e["value"]) for e in log] or any(e["op"] == "O" and e["addr"] == 0xF000 for e in log),
      "device saw the DDRA write (port 0, DDR F0)")
check(any(e["op"] == "O" and e["addr"] == 0xF000 and e["value"] == 0xA5 for e in log), "device saw ORA=A5 with DDR=F0")
# the ROM also initializes the music/game PIA while booting (e.g. CB2), so look at CA2 only
c = [(e["addr"], e["value"]) for e in log if e["op"] == "C" and e["addr"] == 1]
check(c == [(1, 0), (1, 1)], "device saw CA2 go low then high: %s" % c)
check("IRQ source mask: 00000010" in out.split("PIA-IRQ")[1].split("--- a data")[0], "rising CA1 edge (looped back from CA2) raised bus IRQ source 4")
check(re.search(r"PIA A: .*CRA=BF .*CA1=1 .*IRQA=1", out) is not None, "PIA A shows CRA=BF (IRQ1 flag) and IRQA=1")
tail = out.split("PIA-CLEAR")[1]
check(re.search(r"PIA A: .*CRA=3F .*IRQA=0", tail) is not None and "IRQ source mask: 00000000" in tail, "reading the data register cleared the flag and released the IRQ")

# ---------------------------------------------------------------- 3. link loss
print("[3] link loss: device closes the connection")
script = """wait 200
halt
poke 3000 86 11 B7 A7 F0 B7 A7 F1 B7 A7 F2 B7 A7 F3 B7 A7 F4 20 FE
set PC 3000
set CC 10
bp pc 3011
go
waitstop 300
echo AFTER
slots
quit
"""
out, log = run("exp", "--bridge-exp 127.0.0.1:PORT@A7F0+10", script, ["--die-after", "2"])
check("link lost" in out, "the frontend reports the lost link")
check(re.search(r"AFTER\n.*DOWN", out, re.S) is not None, "`slots` shows the bridge as DOWN")
check("STOP: breakpoint at PC=3011" in out, "the emulation went on and reached the breakpoint (no hang)")

print("[4] link stall: device stops answering (timeout path)")
t0 = time.time()
out, log = run("exp", "--bridge-exp 127.0.0.1:PORT@A7F0+10", script, ["--stall-after", "2"])
dt = time.time() - t0
check("link lost" in out and "DOWN" in out, "timeout detected, bridge DOWN")
check("STOP: breakpoint at PC=3011" in out, "the emulation went on (%.1f s total)" % dt)

# ---------------------------------------------------------------- 5. reset + cartridge
print("[5] machine reset reaches the device; cartridge bridge with A7CB")
script = """wait 100
reset
wait 5
halt
poke 3000 86 77 B7 C0 03 B6 C0 03 B7 40 00 B6 A7 CB B7 40 01 20 FE
set PC 3000
set CC 10
bp pc 3010
go
waitstop 300
echo MEM
mem 4000 2
quit
"""
out, log = run("exp", "--bridge-cart 127.0.0.1:PORT@C000+100,ctl", script)
check(any(e["op"] == "X" for e in log), "device received the reset ('X')")
check(re.search(r"MEM\n4000: 77 00", out) is not None, "cartridge window round trip (77) and A7CB served by the device (00)")
check(any(e["op"] == "R" and e["addr"] == 0xA7CB for e in log), "device saw the read of A7CB")

print("ALL PASSED" if not fails else "FAILED (%d)" % fails)
sys.exit(1 if fails else 0)
