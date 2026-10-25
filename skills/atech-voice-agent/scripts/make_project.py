#!/usr/bin/env python3
"""Generate an Atech project for an ElevenLabs voice agent.

Two modes:

  board  The board talks to ElevenLabs by itself over WiFi. Its microphone
         and speaker are the agent's ears and mouth. Needs a mic module and a
         speaker module on the board.

  pc     The computer talks to ElevenLabs (scripts/pc_agent.py). The board
         is connected by USB and is optional: it can play the agent's voice on
         its speaker, show a status light, and give a push-to-talk button.

Examples:

  python3 make_project.py --mode board --dir ~/my-agent --name lumi \
      --agent-id agent_xxx --wifi-ssid MyWifi --wifi-password secret \
      --mic-ports 5,6 --speaker-ports 13,14 --activation name --status-light-port 4

  python3 make_project.py --mode pc --dir ~/my-agent --name lumi \
      --speaker-ports 13,14 --status-light-port 4 --button-port 7
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

ASSETS = Path(__file__).resolve().parent.parent / "assets" / "board_modules"


def ports(text):
    try:
        a, b = (int(p) for p in text.split(","))
    except ValueError:
        raise argparse.ArgumentTypeError("use two neighbouring ports, like 13,14")
    return [a, b]


def c_string(text):
    return text.replace("\\", "\\\\").replace('"', '\\"')


def slug(text):
    return re.sub(r"[^a-z0-9_]", "_", text.lower()).strip("_") or "voice_agent"


# --- code snippets for project.yaml -----------------------------------------

WATCHDOG_SETUP = """\
    // Watchdog: if loop() stops running for 20 s (a hang), restart the board.
    xTaskCreate([](void*) {
      for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (millis() - loopBeatMs > 20000) {
          Serial.println("[Watchdog] loop stuck for 20 s, restarting");
          delay(50);
          ESP.restart();
        }
      }
    }, "loopdog", 2048, nullptr, 1, nullptr);
    loopBeatMs = millis();
"""

SPINNER = """\
    // Status light: a dot circles while {condition}, off otherwise.
    static unsigned long spinAt = 0;
    static int spinPos = 0;
    static bool spinOn = false;
    if ({expr}) {{
      if (millis() - spinAt >= 90) {{
        static const uint8_t ring[8] = {{0, 1, 2, 5, 8, 7, 6, 3}};
        spinAt = millis();
        statuslight.clear();
        statuslight.setPixel(ring[spinPos], 0, 140, 255);
        statuslight.setPixel(ring[(spinPos + 7) % 8], 0, 50, 100);
        statuslight.setPixel(ring[(spinPos + 6) % 8], 0, 15, 30);
        statuslight.show();
        spinPos = (spinPos + 1) % 8;
        spinOn = true;
      }}
    }} else if (spinOn) {{
      statuslight.clear();
      statuslight.show();
      spinOn = false;
    }}
"""

SHOW_TEXT = """\
    // `show` "<text>": word-wrapped text on the screen. Short text big, long text small.
    if (atech_action == "show") {
      String t = atech_value;
      t.trim();
      int size = t.length() <= 26 ? 2 : 1;
      int cols = 156 / (6 * size);
      int y = 4;
      tft.clear();
      tft.setTextSize(size);
      tft.setTextColor(ST7735_TFT::COLOR_WHITE);
      while (t.length() > 0 && y < 80) {
        int n = min((int)t.length(), cols);
        if (n < (int)t.length()) {
          int sp = t.lastIndexOf(' ', n);
          if (sp > 0) n = sp;
        }
        tft.setCursor(4, y);
        tft.print(t.substring(0, n).c_str());
        t = t.substring(n);
        t.trim();
        y += 8 * size + 4;
      }
      tft.display();
      atechDone();
    }
"""


def build_board(args, out):
    mods = out / "my_modules"
    mods.mkdir(parents=True, exist_ok=True)
    for name in ("microphone", "speaker"):
        shutil.copytree(ASSETS / name, mods / name, dirs_exist_ok=True)
    if args.screen_ports:
        # 1-bit canvas version: the stock 25 KB colour buffer doesn't fit next to TLS.
        shutil.copytree(ASSETS / "st7735_tft", mods / "st7735_tft", dirs_exist_ok=True)

    mic = mods / "microphone"
    (mic / "voice_secrets.h").write_text(
        "// Your private settings. Keep this file out of git.\n#pragma once\n\n"
        f'#define VOICE_WIFI_SSID      "{c_string(args.wifi_ssid)}"  // 2.4 GHz only\n'
        f'#define VOICE_WIFI_PASSWORD  "{c_string(args.wifi_password)}"\n'
        f'#define VOICE_AGENT_ID       "{c_string(args.agent_id)}"\n'
    )
    button = args.activation == "button"
    cfg = (mic / "voice_config.h").read_text()
    cfg = re.sub(r"#define VOICE_HANDS_FREE\s+\d", f"#define VOICE_HANDS_FREE      {0 if button else 1}", cfg)
    cfg = re.sub(r"#define VOICE_ALWAYS_ON\s+\d", f"#define VOICE_ALWAYS_ON       {0 if button else 1}", cfg)
    (mic / "voice_config.h").write_text(cfg)

    modules = [f"  - {{id: speaker,    instance: spk,  ports: [{args.speaker_ports[0]}, {args.speaker_ports[1]}]}}",
               f"  - {{id: microphone, instance: mic,  ports: [{args.mic_ports[0]}, {args.mic_ports[1]}]}}"]
    if args.button_port:
        modules.append(f"  - {{id: button,     instance: btn,  port: {args.button_port}}}")
    if args.status_light_port:
        modules.append(f"  - {{id: neopixel,   instance: statuslight, port: {args.status_light_port}}}")
    if args.screen_ports:
        modules.append(f"  - {{id: st7735_tft, instance: tft,  ports: [{args.screen_ports[0]}, {args.screen_ports[1]}]}}")

    setup = WATCHDOG_SETUP
    if args.screen_ports:
        setup += f'    tft.clear();\n    tft.displayText("{c_string(args.agent_name)[:13]}", 4, 28, 2, ST7735_TFT::COLOR_WHITE);\n    tft.display();\n'

    loop = "    loopBeatMs = millis();   // heartbeat for the watchdog\n\n"
    if args.status_light_port:
        loop += SPINNER.format(condition="the agent is connected", expr="voice.isSessionActive()") + "\n"
    if button:
        loop += ("    // Push-to-talk: hold the button and speak, let go to hear the answer.\n"
                 "    if (btn.wasPressed())  voice.startTalking();\n"
                 "    if (btn.wasReleased()) voice.stopTalking();\n\n")
    loop += ("    // `voice_test`: open a session without waiting for speech (for testing).\n"
             '    if (atech_action == "voice_test") {\n      voice.startTalking();\n      atechDone();\n    }\n')
    if args.screen_ports:
        loop += "\n" + SHOW_TEXT

    return modules, setup, loop, True


def build_pc(args, out):
    modules = []
    if args.speaker_ports:
        modules.append(f"  - {{id: speaker,    instance: spk,  ports: [{args.speaker_ports[0]}, {args.speaker_ports[1]}]}}")
    if args.button_port:
        modules.append(f"  - {{id: button,     instance: btn,  port: {args.button_port}}}")
    if args.status_light_port:
        modules.append(f"  - {{id: neopixel,   instance: statuslight, port: {args.status_light_port}}}")
    if args.screen_ports:
        modules.append(f"  - {{id: st7735_tft, instance: tft,  ports: [{args.screen_ports[0]}, {args.screen_ports[1]}]}}")
    if not modules:
        sys.exit("pc mode: nothing to put on the board. Give --speaker-ports, --button-port, "
                 "--status-light-port or --screen-ports, or skip the board entirely.")

    setup = ""
    if args.screen_ports:
        setup += f'    tft.clear();\n    tft.displayText("{c_string(args.agent_name)[:13]}", 4, 28, 2, ST7735_TFT::COLOR_WHITE);\n    tft.display();\n'
    loop = ""
    if args.status_light_port:
        loop += ("    // `agent_status` 1/0 from pc_agent.py: is the agent connected?\n"
                 "    static bool agentOn = false;\n"
                 '    if (atech_action == "agent_status") {\n'
                 "      agentOn = atech_value.toInt() == 1;\n"
                 "      atechDone();\n"
                 "    }\n\n")
        loop += SPINNER.format(condition="pc_agent.py says the agent is connected", expr="agentOn")
    if args.screen_ports:
        loop += "\n" + SHOW_TEXT
    return modules, setup or "    // nothing to set up\n", loop or "    // the computer drives everything\n", False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["board", "pc"], required=True)
    ap.add_argument("--dir", required=True, help="folder to create the project in")
    ap.add_argument("--name", default="voice_agent", help="project name")
    ap.add_argument("--agent-name", default="Agent", help="shown on the screen at boot")
    ap.add_argument("--board", default="14port", choices=["14port", "10port", "8port"])
    ap.add_argument("--speaker-ports", type=ports)
    ap.add_argument("--mic-ports", type=ports)
    ap.add_argument("--button-port", type=int)
    ap.add_argument("--status-light-port", type=int)
    ap.add_argument("--screen-ports", type=ports)
    ap.add_argument("--activation", choices=["name", "always", "button"], default="name")
    ap.add_argument("--agent-id")
    ap.add_argument("--wifi-ssid")
    ap.add_argument("--wifi-password")
    ap.add_argument("--force", action="store_true", help="overwrite an existing project.yaml")
    args = ap.parse_args()

    if args.mode == "board":
        missing = [n for n in ("speaker_ports", "mic_ports", "agent_id", "wifi_ssid", "wifi_password")
                   if not getattr(args, n)]
        if missing:
            sys.exit("board mode needs: " + ", ".join("--" + m.replace("_", "-") for m in missing))
    if args.activation == "button" and not args.button_port:
        sys.exit("--activation button needs --button-port")

    out = Path(args.dir).expanduser().resolve()
    if (out / "project.yaml").exists() and not args.force:
        sys.exit(f"{out}/project.yaml already exists (use --force to overwrite)")
    out.mkdir(parents=True, exist_ok=True)

    build = build_board if args.mode == "board" else build_pc
    modules, setup, loop, uses_custom = build(args, out)

    yaml = [f"name: {slug(args.name)}", f"board: {args.board}"]
    if uses_custom:
        yaml += ["# Voice drivers live in my_modules. WiFi + agent ID: my_modules/microphone/voice_secrets.h",
                 "modules_path: ./my_modules",
                 "# The speaker must stay listed before the microphone (the voice code uses `spk`)."]
    yaml += ["modules:", *modules, "code:", "  setup: |", setup.rstrip("\n"), "  loop: |", loop.rstrip("\n"), ""]
    (out / "project.yaml").write_text("\n".join(yaml))
    (out / ".gitignore").write_text("build/\nmy_modules/microphone/voice_secrets.h\n")

    print(f"Created {out}/project.yaml ({args.mode} mode)")
    print("Next: atech validate . && atech upload .   (run inside that folder)")


if __name__ == "__main__":
    main()
