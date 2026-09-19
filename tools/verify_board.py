#!/usr/bin/env python3
"""verify_board.py - prove a freshly flashed board works, over the USB cable.

Connects to the firmware's serial console and runs three checks:

  1. status     the firmware answers and reports its module type and mode
  2. self-test  the 9-step display test (watch the strip while it runs)
  3. timer      a short countdown; every one of the 256 bar steps must arrive
                and the timing must be exact

Usage:
    python verify_board.py                 # all checks, port auto-detected
    python verify_board.py --port COM7
    python verify_board.py --skip-selftest --timer 5

Exit code 0 means PASS. Requires: pip install pyserial
"""

import argparse
import re
import sys
import time

ESP32_USB_VID = 0x303A


def find_port(explicit):
    from serial.tools import list_ports
    if explicit:
        return explicit
    for p in list_ports.comports():
        if p.vid == ESP32_USB_VID or "303A" in (p.hwid or "").upper():
            return p.device
    found = ", ".join(p.device for p in list_ports.comports()) or "none"
    sys.exit(f"FAIL: no ESP32-C3 serial port found (ports seen: {found}). "
             "Plug the board in, or pass --port COMx.")


class Console:
    def __init__(self, port):
        import serial
        self.ser = serial.Serial(port, 115200, timeout=0.1, dsrdtr=False, rtscts=False)
        self.ser.rts = False          # low so the board is not reset
        self.ser.dtr = False
        time.sleep(0.3)
        self.ser.reset_input_buffer()

    def send(self, line):
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode("utf-8"))

    def read_for(self, seconds):
        end = time.time() + seconds
        buf = b""
        while time.time() < end:
            buf += self.ser.read(4096)
        return buf.decode("utf-8", "replace").splitlines()

    def close(self):
        self.ser.close()


def check_status(con):
    con.send("s")
    lines = con.read_for(0.6)
    status = next((l for l in lines if l.startswith("mode=")), None)
    if not status:
        print("  FAIL: no answer from the firmware. Is this the right port, and is the LED matrix firmware flashed?")
        return False
    print("  " + status)
    return True


def check_selftest(con):
    print("  running the 9-step display test, watch the strip (about 18 s)...")
    con.send("d")
    steps = 0
    for line in con.read_for(20):
        if line.startswith("Self-test"):
            steps += 1
            print("  " + line)
    ok = steps == 9
    print("  PASS: all 9 steps ran" if ok else f"  FAIL: expected 9 steps, saw {steps}")
    return ok


def check_timer(con, seconds):
    print(f"  starting a {seconds} s timer, checking every bar step...")
    con.send(f"t {seconds}")
    lines = con.read_for(seconds + 3)
    steps = [(int(m.group(1)), int(m.group(2))) for l in lines
             for m in [re.search(r"\[(\d+) ms\] bar (\d+)/256", l)] if m]
    done = [l for l in lines if "Mode: done" in l]
    con.send("c")                                   # back to the resting screen
    con.read_for(0.3)

    problems = []
    if len(steps) != 256:
        problems.append(f"expected 256 bar steps, saw {len(steps)}")
    if steps and [s[1] for s in steps] != list(range(256)):
        problems.append("bar steps were not 0..255 in order")
    if not done:
        problems.append("no 'Mode: done' line at the end")
    if len(steps) > 2:
        span = steps[-1][0] - steps[0][0]
        expected = seconds * 1000 * 255 / 256
        if abs(span - expected) > expected * 0.05 + 100:
            problems.append(f"timing off: 255 steps took {span} ms, expected about {int(expected)} ms")
        else:
            print(f"  255 steps in {span} ms (expected about {int(expected)} ms)")
    for p in problems:
        print("  FAIL: " + p)
    if not problems:
        print("  PASS: 256 steps, exact timing, done on time")
    return not problems


def main():
    ap = argparse.ArgumentParser(description="Check a flashed ESP32-C3 LED matrix over USB.")
    ap.add_argument("--port", help="serial port, e.g. COM7 (default: auto-detect)")
    ap.add_argument("--timer", type=int, default=10, help="timer length in seconds for the timing check (default 10)")
    ap.add_argument("--skip-selftest", action="store_true", help="do not run the display self-test")
    args = ap.parse_args()

    port = find_port(args.port)
    print(f"Board on {port}")
    con = Console(port)
    results = []

    print("\n[1/3] Firmware status")
    results.append(check_status(con))
    if not results[0]:
        con.close()
        sys.exit(1)

    print("\n[2/3] Display self-test")
    results.append(True if args.skip_selftest else check_selftest(con))
    if args.skip_selftest:
        print("  skipped")

    print("\n[3/3] Timer accuracy")
    results.append(check_timer(con, args.timer))
    con.close()

    print("\nRESULT: " + ("PASS - the board is working" if all(results) else "FAIL - see the lines above"))
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
