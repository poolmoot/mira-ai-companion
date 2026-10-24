# Architecture

```
 you ──voice──► [Atech board] ══WiFi / TLS WebSocket══► ElevenLabs agent "Mira"
                mic 5-6 (μ-law 8 kHz up)                 Scribe STT → Claude Haiku → Emma TTS
                speaker 13-14 (PCM 16 kHz down)                │
                lights 1-4, button 7                           │ webhook tools (HTTPS)
                screen 9-10, temp 11                           ▼
                ══USB serial══════════════════════► ┌──────────────────┐  REST /api/*  + WS /ws
                                                    │  hub.py  :8000   │ ◄──────────────────────── dashboard
                                                    │  FastAPI+SQLite  │
                                                    └──────────────────┘
                                                      ▲ Cloudflare quick tunnel (public URL)
```

## The three parts

**The board** (`firmware/`) runs the voice loop by itself. Its mic streams to the
ElevenLabs agent over a TLS WebSocket and the agent's audio plays on its speaker. The
session opens at boot and stays open. Port 4's light circles while it is connected.
Lights 1-3 and the button handle the medicine reminder on the board itself, so they
keep working if the laptop is slow.

**The hub** (`hub/hub.py`) is the only program on the board's USB port. It runs the
medicine schedule, stores everything in SQLite, and is the one API that both the
agent (webhook tools under `/tools/*`) and a dashboard (`/api/*` and `/ws`) use.

**The agent** (`elevenlabs/`) is a public ElevenLabs Conversational AI agent: Scribe
speech-to-text, Claude Haiku as the brain, Emma's voice. Its tools reach the hub
through a Cloudflare quick tunnel that `start.sh` opens.

## Board protocol (USB serial, via the Atech SDK)

| Action | Value | Effect |
|---|---|---|
| `med_due` | text, or `0` | lights 1-3 red, chime, text on screen; `0` cancels |
| `show` | text | word-wrapped text on the screen |
| `lights` | `r,g,b` | lights 1-3 |
| `med_state` | | replies with a `med_state` event |
| `voice_test` | | opens a voice session without waiting for speech |

| Event | Meaning |
|---|---|
| `med_taken` | button pressed while medicine was due |
| `btn` | every button press and release |
| `temp_temperature`, `temp_humidity` | every 2 s |
| `mic_level` | mic RMS level every 250 ms |

## Hub API (full schema: [openapi.json](openapi.json), or http://localhost:8000/docs)

| Method | Path | What |
|---|---|---|
| GET | /api/state | live state: med_due, med_name, next_med_time, temperature, humidity, screen_text, med_times |
| GET | /api/meds | medicine log `{id, due_at, taken_at, source}` (source: button / voice / dashboard) |
| GET | /api/moods | mood log `{id, at, mood, score 1-5, note}` |
| GET | /api/messages | texts Mira "sent" (demo) `{id, at, recipient, message}` |
| GET | /api/readings | temperature/humidity history |
| PUT | /api/schedule `{"times":["08:00","20:00"]}` | when the medicine light turns red |
| POST | /api/med/due | turn the reminder on now |
| POST | /api/med/taken | log a dose from the dashboard |
| POST | /api/screen `{"text":"..."}` | text on the device screen |
| POST | /api/lights `{"r":0,"g":0,"b":255}` | set lights 1-3 |
| WS | /ws | `{event, data, state, at}`; events: hello, med_due, med_cleared, med_taken, mood, reading, button, screen, schedule, message_sent |

Agent tools (return `{"result": "..."}`): `medication_status`, `log_medication`,
`log_mood {mood, score, note}`, `room`, `show_on_screen {text}`, `send_message {recipient, message}`.

## Why the firmware looks the way it does

The ESP32-S3 has about 130 KB of usable heap here, and one TLS session needs about
47 KB of it. Every design choice below came from a measured failure:

| Problem I hit | Fix |
|---|---|
| Speech came through at −60 dBFS, so the agent heard nothing | keep all 24 bits from the ICS-43434 mic and apply ×16 gain |
| Agent audio frames are up to ~37 KB, the WebSocket library caps at 15 KB (close 1009) | vendored the library with a 48 KB limit |
| Allocating that frame on a fragmented heap failed mid-reply (close 1011) | one frame buffer reserved at boot and reused |
| After a session the heap is too fragmented for a new TLS handshake | a failed reconnect reboots the board: clean heap, back in ~10 s |
| 16 kHz PCM upload over a phone hotspot stalled the main loop for 5-12 s | send 8 kHz μ-law instead: 4× less upload |
| The board's playback timer misread backpressure as "reply finished", so the mic reopened and the agent interrupted itself | hold the timer while the board itself pauses reading, and mute the mic for 400 ms after replies |
| The 25 KB colour screen buffer didn't fit next to TLS | a 1-bit canvas (1.6 KB) pushed in bands |
| A rare hang left the device silent | a watchdog task reboots it if the main loop stops for 20 s |
