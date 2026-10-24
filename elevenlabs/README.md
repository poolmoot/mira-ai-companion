# The Mira agent (ElevenLabs)

Everything you need to recreate Mira in your own ElevenLabs workspace.

| Setting | Value | Why |
|---|---|---|
| LLM | `claude-haiku-4-5` | fast first token for voice |
| Voice | Emma, `56bWURjYFHyYyVf490Dp` | warm, conversational |
| TTS model | `eleven_v4_turbo` | low latency |
| Agent output audio | `pcm_16000` | what the board's I2S speaker plays |
| User input audio | `ulaw_8000` | 4× smaller uplink, so a phone hotspot keeps up |
| Turn eagerness | `eager`, speculative turn on | answers sooner |
| Background voice detection | on | ignores other people in the room |
| System tool | `skip_turn` enabled | stays quiet when not addressed |
| Authentication | public agent | the board connects with only the agent ID |

## Steps

1. Create an agent and paste [`prompt.md`](prompt.md) as its system prompt.
2. Apply the settings in the table above.
3. Run `./start.sh` in the repo root and copy the public URL it prints.
4. Create the six webhook tools in [`tools.json`](tools.json), replacing
   `https://YOUR-TUNNEL.trycloudflare.com` with that URL, and attach them to the agent.
5. Put the agent ID in `firmware/my_modules/microphone/voice_secrets.h`.

When the tunnel restarts it gets a new URL. `ELEVENLABS_API_KEY=... python3 hub/retarget_tools.py <url>`
points every `/tools/*` webhook at the new one (`start.sh` does this for you when the key is set).
