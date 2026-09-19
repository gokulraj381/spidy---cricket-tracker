#!/usr/bin/env python3
"""Copy src/main.cpp to the Arduino IDE sketch so both stay identical.

Run after editing the firmware:  python tools/sync_ino.py
"""
import pathlib
import shutil

root = pathlib.Path(__file__).resolve().parent.parent
src = root / "src" / "main.cpp"
dst = root / "arduino" / "ESP32C3_LED_MATRIX" / "ESP32C3_LED_MATRIX.ino"
dst.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(src, dst)
print(f"copied {src.relative_to(root)} -> {dst.relative_to(root)} ({src.stat().st_size} bytes)")
