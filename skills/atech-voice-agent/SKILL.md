---
name: atech-voice-agent
description: Build a talking AI voice agent (an ElevenLabs Conversational AI agent) on an Atech board, with the board's microphone and speaker, from zero to a working device. Use this whenever someone has an Atech kit and wants a voice assistant, AI companion, talking robot or "Alexa-like" device, wants their Atech board to listen and talk, or wants to connect an Atech board to ElevenLabs, even if they don't say "skill" or "agent". It asks the user for the agent's name, how to wake it, where it should run (on the board over WiFi, or through the computer over USB) and which ports the mic and speaker are in, then creates the agent, generates and flashes the firmware, and checks it works. Every known pitfall (silent mic, audio formats, memory, reconnects, echo) is already fixed in the bundled code.
---

# Atech voice agent

This skill turns an Atech board into a voice agent the user can talk to. It packs the
lessons from building the first one (a device called Mira) into ready-made code, so
the user only answers a few questions.

The user is often not an engineer. Use plain words, one step at a time, and say what
each step is for. Never paste long code at them; run things yourself.

## What gets built

There are two ways to run it. Explain both in one sentence each, then let the user pick:

- **On the board** (WiFi): the board talks to ElevenLabs by itself. After setup it works
  with just power and WiFi, no computer. Needs a microphone and a speaker module.
- **Through the computer** (USB cable): the computer talks to ElevenLabs and uses its
  own microphone. The agent's voice can play on the board's speaker or the computer's.
  More robust on bad WiFi, and no memory limits, but the computer must stay on.

If they can't decide: on the board for a "real device" feel, through the computer if
their WiFi is poor or they have no mic module.

## Step 1: Check the basics

Run these and fix what's missing before asking anything:

```bash
python3 -m pip show atech >/dev/null || python3 -m pip install atech
atech ports          # is a board plugged in? (USB data cable, not a charge-only one)
atech list-boards    # which boards exist; ask which one they have if unsure
```

For PC mode also: `python3 -m pip install elevenlabs sounddevice numpy`.

## Step 2: Ask the user

Ask with the AskUserQuestion tool where there are clear options, and in plain text for
names. Group related questions. Collect:

1. **Agent name**: what they'll call it, e.g. "Lumi", "Max". Suggest a short, clear
   name with 2 syllables, because speech-to-text mishears unusual names.
2. **What it's for**, in one line: a friendly companion, a kitchen helper, a study
   buddy. This becomes the agent's personality. Also ask the language.
3. **How to wake it**:
   - *Say its name* (recommended): always listening, answers when it hears its name.
   - *Always listening*: answers everything. Fine in a quiet room.
   - *Push a button*: hold the button and speak. Most reliable in a loud room.
4. **Where it runs**: on the board, or through the computer (see above).
5. **Voice**: female/male/neutral and a feel (warm, calm, upbeat). You'll pick a matching voice.
6. **Ports**. Tell them to look at the board: each module sits in numbered ports.
   - Speaker: a pair of neighbouring ports (e.g. 13 and 14). Required on the board;
     in PC mode only if they want the voice on the board's speaker.
   - Microphone: a pair of neighbouring ports (e.g. 5 and 6). Board mode only.
   - Button port, if they wake it with a button (or want one anyway).
   - Optional: a light grid for a **status light** (a light circles while the agent is
     connected; this is the user's way to see the agent is alive) and a screen.
   Don't guess ports. If they're unsure, have them read the numbers printed next to
   the modules on the board.
7. **Board mode only: WiFi** name and password. It must be a **2.4 GHz** network; the
   board can't see 5 GHz. On an iPhone hotspot, turn on "Maximise Compatibility".
   Tell them it's stored only in a local file that's excluded from git.
8. Where to put the project folder (default: `~/<name>-voice-agent`).

## Step 3: Create the ElevenLabs agent

Read `references/elevenlabs-agent.md` and follow it. In short:

- If the ElevenLabs connector (MCP) is available, create and configure the agent with
  it. If not, offer to connect it (it saves the user a lot of clicking), or walk them
  through the dashboard steps one at a time.
- The **user input audio format must match the mode**: `ulaw_8000` on the board,
  `pcm_16000` through the computer. Wrong format = the agent hears noise and silently
  understands nothing. This was the hardest bug to find, so double-check it.
- Authentication off (the board connects with only the agent ID), first message empty.
- Before any hardware, suggest they press **Test AI agent** in the ElevenLabs
  dashboard and talk to it from the browser. That proves the agent itself works.

## Step 4: Generate the project

Use the bundled generator. It writes `project.yaml`, copies the voice drivers, puts
the WiFi password and agent ID in a git-ignored `voice_secrets.h`, and wires up the
status light, button and screen.

```bash
SKILL_DIR=<this skill's folder>
# Board mode:
python3 "$SKILL_DIR/scripts/make_project.py" --mode board --dir ~/lumi-voice-agent \
  --name lumi --agent-name Lumi --agent-id agent_xxx \
  --wifi-ssid "WiFi name" --wifi-password "password" \
  --mic-ports 5,6 --speaker-ports 13,14 --activation name \
  --status-light-port 4            # optional: --button-port 7 --screen-ports 9,10
# PC mode (only the board parts you'll use):
python3 "$SKILL_DIR/scripts/make_project.py" --mode pc --dir ~/lumi-voice-agent \
  --speaker-ports 13,14 --status-light-port 4   # optional: --button-port 7 --screen-ports 9,10
```

`--activation button` requires `--button-port`. In PC mode, skip the board entirely if
they use the computer's speaker and want no light or button.

Then, inside the project folder:

```bash
atech validate .     # checks the ports; says which pairs work if one is wrong
atech upload .       # builds and flashes (first build downloads ~2.4 GB, once)
```

If `validate` complains about a port, ask the user to re-check where the module sits;
don't silently move it. If `upload` hangs at "Connecting...", run it again.

## Step 5: Test it with the user

**Board mode**: run `python3 "$SKILL_DIR/scripts/watch_board.py" --seconds 60` and, while
it runs, ask the user to say "{name}, how are you?" near the device. The script prints
"WiFi connected", "Agent connected", what it heard and what the agent said, and a
verdict with the mic level. The status light should circle once the agent is connected.

**PC mode**:

```bash
python3 "$SKILL_DIR/scripts/pc_agent.py" --agent-id agent_xxx \
  --speaker board --status-light        # or --speaker pc; --activation button
```

It prints what it heard and what the agent said. The first run on macOS asks for
microphone permission. A headset avoids echo when using the computer's speakers.

If something's off, read `references/troubleshooting.md`; most problems are there
with the fix. Talk the user through it in plain words.

## Step 6: Hand over

Tell the user, briefly:
- How to talk to it, and what the status light means.
- Where the project lives, and that `voice_secrets.h` holds their WiFi password and
  agent ID: keep it out of public repos (the agent is public, so anyone with the ID
  can use their ElevenLabs minutes).
- How to change things later: the agent's personality and voice in ElevenLabs (it
  takes effect on the next reconnect; restart the board to force it), loudness in
  `my_modules/microphone/voice_config.h`, then `atech upload .` again.
- Board mode keeps the session open all the time, so it uses ElevenLabs minutes while
  powered on.

## Things to keep in mind

- **Less-proven paths.** The board-mode drivers run on a real device, and PC mode with
  the computer's mic and speakers was tested end to end. Two paths have been built
  and compiled but not yet run on hardware: flashing a *generated* board-mode
  project, and PC mode with `--speaker board`. On those, watch the first run
  closely (`watch_board.py`, or the PC script's output) and tell the user if
  something behaves differently from these notes.

- **Board mode is at the edge of the chip's memory.** One encrypted connection needs
  ~47 KB of the ~130 KB available. The generated project fits with a status light,
  a button and a screen. Don't add other modules or features to the board-mode
  firmware; if the user wants more, suggest PC mode or a second board.
- **Keep the board-mode drivers as they are.** Every odd-looking choice in
  `assets/board_modules` (24-bit mic gain, μ-law upload, a reserved 48 KB frame
  buffer, reboot on a failed reconnect, the 1-bit screen canvas) fixes a failure that
  really happened; `references/troubleshooting.md` lists them.
- **Tools that do things** (log a dose, send a message, read a sensor) are a next
  step, not part of this setup. Board mode needs a small web server the agent can
  reach (a webhook tool behind a tunnel). PC mode can use client tools, which run
  inside `pc_agent.py`. See the comment in `pc_agent.py`.
