# Troubleshooting

Every row here is a real failure from building the first device, with what fixed it.
The generated project already includes all these fixes; this table is for when
something still goes wrong. Most board problems show up in `scripts/watch_board.py`.

## Board mode

| What you see | Cause | Fix |
|---|---|---|
| Agent connects, but conversations have **0 messages** | input audio format doesn't match, or the mic is silent | agent input must be `ulaw_8000`; check the mic ports; `watch_board.py` shows the mic level (speech should reach several hundred to thousands) |
| Mic level stays under ~300 when talking | mic in the wrong ports, or too far away | swap to the right adjacent port pair; speak within ~1 m; raise `MIC_GAIN` in `microphone.h` (default 16) |
| `WiFi failed` | wrong name/password, or a 5 GHz network | the ESP32 only sees **2.4 GHz**. Phone hotspots: turn on "maximise compatibility" (iPhone) |
| Connects, then drops every few seconds, `SEND FAILED` / `Connection lost` | upload too slow (busy WiFi, hotspot) | already sending μ-law 8 kHz; move closer to the router, or use the PC mode |
| `Reconnect failed ... restarting the board` | heap too fragmented for a new TLS handshake | expected and self-healing: the board reboots and is back in ~10 s |
| `[WS] no memory for a ... B frame` / close code 1011 | not enough contiguous RAM for an agent audio frame | remove extra modules; don't add features to the voice firmware |
| Close code 1009 | agent audio frame bigger than the WebSocket limit | the bundled library allows 48 KB; raise `WEBSOCKETS_MAX_DATA_SIZE` in `WebSockets.h` only if you have RAM to spare |
| Agent cuts itself off mid-sentence | the agent hears its own voice | already handled (mic muted while playing + 400 ms); keep the speaker pointed away from the mic |
| Voice too quiet / distorted | gain | `VOICE_SPEAKER_GAIN` (default 2.5) and `VOICE_SPEAKER_VOLUME` (0.9) in `voice_config.h` |
| Device goes completely silent, status light off | the agent session is down | the board reconnects by itself; if it stays off >30 s, check WiFi with `watch_board.py` |
| Agent answers TV and other people | activation "always", or the prompt is too loose | use activation "say the name" + `skip_turn`, and turn on background voice detection |
| Agent ignores its name | name misheard by speech-to-text | add spelling variants to the prompt (`Mirra, Meera, Myra`), or pick a clearer name |
| `atech upload` hangs at `Connecting......` | the board has no valid program after an interrupted flash | run `atech upload .` again; it recovers |
| Upload/monitor says the port is busy | another program has the USB port | close `atech monitor`, `watch_board.py`, or anything else using it; `atech free-port` |

## PC mode

| What you see | Cause | Fix |
|---|---|---|
| Agent never hears you | input format isn't `pcm_16000`, or the wrong microphone | set the agent's input to PCM 16000; macOS asks for mic permission for the Terminal the first time |
| Agent interrupts itself | it hears itself through the speakers | use a headset, or lower the volume; the script mutes the mic while the agent talks |
| `Could not connect` | private agent without a key | set `ELEVENLABS_API_KEY`, or turn authentication off |
| Board speaker stutters | USB chunks arriving late | close other programs using the board; the script sends ~64 ms chunks |

## Atech basics that bite

- One program per board port at a time. `atech monitor`, `watch_board.py`, `pc_agent.py`
  and `atech upload` can't run together.
- Two-port modules need two **neighbouring** ports, like 5-6 or 13-14. `atech validate`
  says which pairs work.
- The first `atech build` downloads the ESP32 compiler (~2.4 GB, once).
- In board mode the speaker must be listed before the microphone in `project.yaml`
  (the generator does this).
