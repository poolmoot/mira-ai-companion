#!/usr/bin/env python3
"""Watch a board-mode voice agent and say in plain words whether it works.

  python3 watch_board.py              # 40 s, restarts the board first
  python3 watch_board.py --seconds 90 --no-reset

Close anything else using the board's port first (atech monitor, the hub).
"""

import argparse
import json
import sys
import time

import serial
from serial.tools import list_ports


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x303A:          # Espressif: the Atech board's USB
            return p.device
    sys.exit("No Atech board found on USB. Plug it in with a data cable (atech ports).")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=40)
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("--port")
    args = ap.parse_args()

    s = serial.Serial(args.port or find_port(), 115200, timeout=0.5)
    if not args.no_reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
        print("Restarted the board. Watching...\n")

    seen = {"wifi": False, "ready": False, "heard": 0, "said": 0, "drops": 0, "reboots": 0}
    levels = []
    t0 = time.time()
    while time.time() - t0 < args.seconds:
        line = s.readline().decode(errors="replace").strip()
        if not line:
            continue
        t = f"{time.time() - t0:5.1f}s"
        if '"mic_level"' in line:
            try:
                levels.append(json.loads(line)["payload"]["value"])
            except (ValueError, KeyError):
                pass
            continue
        if "WiFi connected" in line:
            seen["wifi"] = True
            print(f"{t}  WiFi connected")
        elif "WiFi failed" in line:
            print(f"{t}  WiFi FAILED: check the name/password, and that it is a 2.4 GHz network")
        elif "Conversation ready" in line:
            seen["ready"] = True
            print(f"{t}  Agent connected")
        elif "] You:" in line:
            seen["heard"] += 1
            print(f"{t}  heard: {line.split('You:', 1)[1].strip()}")
        elif "] Agent:" in line:
            seen["said"] += 1
            print(f"{t}  agent: {line.split('Agent:', 1)[1].strip()}")
        elif "Disconnected" in line:
            seen["drops"] += 1
            print(f"{t}  connection dropped ({line.split('(', 1)[-1][:40]}")
        elif "restarting" in line.lower() or "Watchdog" in line:
            seen["reboots"] += 1
            print(f"{t}  board restarting to recover")
        elif "no memory" in line:
            print(f"{t}  OUT OF MEMORY: {line}")

    print("\n--- verdict ---")
    print("WiFi:   ", "ok" if seen["wifi"] else "not seen (it may have connected before watching; use reset)")
    print("Agent:  ", "connected" if seen["ready"] else "NOT connected")
    print("Heard:  ", seen["heard"], "phrases   Replied:", seen["said"])
    if levels:
        quiet = sorted(levels)[len(levels) // 2]
        print(f"Mic:     median level {quiet}, loudest {max(levels)} (0-32767)")
        if max(levels) < 300:
            print("         The mic looks silent. Check the mic ports, or raise MIC_GAIN in microphone.h.")
    if seen["drops"]:
        print("Drops:  ", seen["drops"], "(a few are normal on a phone hotspot; the board reconnects)")


if __name__ == "__main__":
    main()
