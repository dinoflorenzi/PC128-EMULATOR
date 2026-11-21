#!/usr/bin/env python3
"""Reference/test implementation of the device side of the bridge protocol
(see frontend/BRIDGE_PROTOCOL.md).

  fake_micro.py exp  --port 5555 --log ops.jsonl
      16 registers at addr & 15. Writing a nonzero value to register 14 asserts IRQ,
      reading register 14 releases it. Register 15 reads back 0xC3 regardless (a fixed ID).
  fake_micro.py joy  --port 5556 --log ops.jsonl --pins-a 3c --pins-b ff
      Answers 'I' with fixed pin values. CA2 driven by the emulator ('C' op) is looped
      back to the CA1 input and reported in the control-line flags.

Every request is appended to --log as one JSON object per line.
  --die-after N   close the connection after N requests (to test the link-loss path)
  --stall-after N stop answering after N requests but keep the connection open (timeout path)
  --delay MS      sleep MS milliseconds before every answer (simulates a slow link)
"""
import argparse
import json
import socket
import struct
import sys
import time


def serve(args):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(1)
    print("listening on 127.0.0.1:%d (%s)" % (args.port, args.kind), flush=True)
    conn, _ = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    log = open(args.log, "w") if args.log else None
    regs = [0] * 16
    irq = 0
    ca1 = 1          # loopback of CA2
    n = 0
    pins = {0: int(args.pins_a, 16), 1: int(args.pins_b, 16)}

    def readn(k):
        buf = b""
        while len(buf) < k:
            chunk = conn.recv(k - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    while True:
        frame = readn(12)
        if frame is None:
            break
        op, value, addr, cycle = struct.unpack("<cBHQ", frame)
        op = op.decode()
        n += 1
        if log:
            log.write(json.dumps({"op": op, "value": value, "addr": addr, "cycle": cycle}) + "\n")
            log.flush()
        if args.stall_after and n > args.stall_after:
            time.sleep(30)
            break
        data = 0
        flags = 0
        if args.kind == "exp":
            if op == "R":
                r = addr & 15
                if r == 15:
                    data = 0xC3
                else:
                    data = regs[r]
                    if r == 14:
                        irq = 0
            elif op == "W":
                regs[addr & 15] = value
                if (addr & 15) == 14 and value:
                    irq = 1
            elif op == "X":
                regs = [0] * 16
                irq = 0
            flags = irq
        else:  # joy
            if op == "I":
                data = pins[addr & 1]
            elif op == "C" and addr == 1:   # CA2 output -> CA1 input
                ca1 = value & 1
            elif op == "X":
                ca1 = 1
            # control lines: CA1 = loopback, the others idle high
            flags = 0x80 | (ca1 << 2) | (1 << 3) | (1 << 4) | (1 << 5) | irq
        if args.delay:
            time.sleep(args.delay / 1000.0)
        try:
            conn.sendall(bytes([data, flags]))
        except OSError:
            break
        if args.die_after and n >= args.die_after:
            break
    conn.close()
    srv.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("kind", choices=["exp", "joy"])
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--log")
    ap.add_argument("--pins-a", default="ff")
    ap.add_argument("--pins-b", default="ff")
    ap.add_argument("--die-after", type=int, default=0)
    ap.add_argument("--stall-after", type=int, default=0)
    ap.add_argument("--delay", type=float, default=0)
    serve(ap.parse_args())
