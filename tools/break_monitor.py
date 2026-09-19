#!/usr/bin/env python3
"""break_monitor.py - nag the LED matrix when you have worked too long.

Watches keyboard and mouse activity on this Windows PC. After WORK minutes of
continuous work it sends an alert to the ESP32 LED matrix, which flashes and
then scrolls the message until you have been away from the PC for BREAK
minutes. Short pauses under BREAK minutes still count as working. Once you
have taken the break, the alert is cleared and the work timer starts over.
While you keep working through the alert it is re-sent every NAG minutes, so
the matrix flashes again.

Talks to the board over the USB serial console by default (the same cable that
powers it), so no Wi-Fi is needed. Use --http 192.168.4.1 if the PC is joined
to the board's Wi-Fi instead.

Usage:
    python break_monitor.py                         # 15 min work, 3 min break, port auto-detected
    python break_monitor.py --work 45 --break 5
    python break_monitor.py --message "TAKE A BREAK" --port COM7
    python break_monitor.py --http 192.168.4.1

Requires: pip install pyserial   (only for the serial transport)
Stop with Ctrl+C. The alert, if showing, is cleared on exit.
"""

import argparse
import ctypes
import sys
import time
import urllib.parse
import urllib.request

ESP32_USB_VID = 0x303A   # Espressif's USB vendor id (built-in USB-Serial-JTAG)


# ── Windows idle time ────────────────────────────────────────────────────────

class _LASTINPUTINFO(ctypes.Structure):
    _fields_ = [("cbSize", ctypes.c_uint), ("dwTime", ctypes.c_uint)]


def idle_seconds():
    """Seconds since the last keyboard or mouse event, from the Windows API."""
    info = _LASTINPUTINFO()
    info.cbSize = ctypes.sizeof(_LASTINPUTINFO)
    if not ctypes.windll.user32.GetLastInputInfo(ctypes.byref(info)):
        return 0.0
    now = ctypes.windll.kernel32.GetTickCount()
    return ((now - info.dwTime) & 0xFFFFFFFF) / 1000.0


# ── Links to the board ───────────────────────────────────────────────────────

def log(msg):
    print(f"\r{time.strftime('%H:%M:%S')}  {msg}".ljust(78))


def status(msg):
    print(f"\r{time.strftime('%H:%M:%S')}  {msg}".ljust(78), end="", flush=True)


class SerialLink:
    """USB serial console of the firmware: 'a TEXT' shows an alert, 'k' clears it."""

    def __init__(self, port=None):
        self.port = port
        self.ser = None

    def _open(self):
        import serial
        from serial.tools import list_ports

        port = self.port
        if not port:
            for p in list_ports.comports():
                hwid = (p.hwid or "").upper()
                if p.vid == ESP32_USB_VID or f"VID:PID={ESP32_USB_VID:04X}" in hwid or "VID_303A" in hwid:
                    port = p.device
                    break
            if not port:
                found = ", ".join(p.device for p in list_ports.comports()) or "none"
                raise RuntimeError(f"no ESP32-C3 serial port found (ports: {found}); "
                                   "is the USB cable plugged in? Or pass --port COMx")
        # dtr/rts low so opening the port never resets the board
        self.ser = serial.Serial(port, 115200, timeout=0.2, dsrdtr=False, rtscts=False)
        self.ser.rts = False
        self.ser.dtr = False
        self.port = port
        log(f"serial connected on {port}")

    def send(self, line):
        for _attempt in range(2):
            try:
                if self.ser is None:
                    self._open()
                self.ser.reset_input_buffer()
                self.ser.write((line + "\n").encode("utf-8"))
                time.sleep(0.3)
                return self.ser.read(2048).decode("utf-8", "replace").strip()
            except Exception as exc:  # port vanished (board reset or unplugged): reconnect
                log(f"serial error: {exc}")
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
                time.sleep(1.0)
        return None

    def alert(self, message):
        return self.send("a " + message)

    def clear(self):
        return self.send("k")


class HttpLink:
    """The board's web endpoints, for a PC that is on the board's Wi-Fi."""

    def __init__(self, host):
        self.base = f"http://{host}"

    def _get(self, path):
        try:
            with urllib.request.urlopen(self.base + path, timeout=3) as resp:
                return resp.read().decode("utf-8", "replace")
        except Exception as exc:
            log(f"http error: {exc}")
            return None

    def alert(self, message):
        return self._get("/alert?text=" + urllib.parse.quote(message))

    def clear(self):
        return self._get("/clearAlert")


# ── Main loop ────────────────────────────────────────────────────────────────

def fmt(seconds):
    seconds = int(seconds)
    return f"{seconds // 60:02d}:{seconds % 60:02d}"


def main():
    ap = argparse.ArgumentParser(description="Nag the LED matrix after too much continuous work.")
    ap.add_argument("--work", type=float, default=15, help="minutes of work before the alert (default 15)")
    ap.add_argument("--break", dest="brk", type=float, default=3,
                    help="minutes away from the PC that count as a break and reset the timer (default 3)")
    ap.add_argument("--nag", type=float, default=2,
                    help="minutes between repeat flashes while you keep working, 0 = only once (default 2)")
    ap.add_argument("--message", default="BITCH TAKE A BREAK", help="text to scroll on the matrix")
    ap.add_argument("--port", default=None, help="serial port, e.g. COM7 (default: auto-detect the ESP32-C3)")
    ap.add_argument("--http", default=None, metavar="HOST", help="use the web endpoints at HOST instead of serial")
    ap.add_argument("--poll", type=float, default=5, help="seconds between activity checks (default 5)")
    args = ap.parse_args()

    if sys.platform != "win32":
        sys.exit("This monitor uses the Windows idle-time API and only runs on Windows.")

    link = HttpLink(args.http) if args.http else SerialLink(args.port)
    work_s, break_s, nag_s = args.work * 60, args.brk * 60, args.nag * 60

    session_start = None   # when the current stretch of work began
    alert_on = False
    last_sent = 0.0

    log(f"watching this PC: alert after {args.work:g} min of work, "
        f"cleared after {args.brk:g} min away, message \"{args.message}\"")

    try:
        while True:
            idle = idle_seconds()
            now = time.time()

            if idle >= break_s:
                # A real break: clear the nag and start counting again next time
                if alert_on:
                    link.clear()
                    alert_on = False
                    log("break taken - alert cleared, work timer reset")
                elif session_start is not None:
                    log("away from the PC - work timer reset")
                session_start = None
                status(f"on a break, {fmt(idle)} away")
            else:
                if session_start is None:
                    session_start = now - idle
                    log("working...")
                worked = now - session_start
                if worked >= work_s:
                    if not alert_on or (nag_s > 0 and now - last_sent >= nag_s):
                        link.alert(args.message)
                        alert_on = True
                        last_sent = now
                        log(f"{fmt(worked)} of work - alert sent to the matrix")
                    status(f"OVERDUE {fmt(worked)} - alert showing, step away for {args.brk:g} min")
                else:
                    status(f"working {fmt(worked)} / {fmt(work_s)}   (last input {int(idle)} s ago)")

            time.sleep(args.poll)
    except KeyboardInterrupt:
        print()
        if alert_on:
            link.clear()
            log("alert cleared")
        log("stopped")


if __name__ == "__main__":
    main()
