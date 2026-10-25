# Creating the ElevenLabs agent

Contents: 1. Settings per mode · 2. The prompt template · 3. Path A: with the ElevenLabs connector (MCP) · 4. Path B: by hand in the dashboard · 5. Checking it

## 1. Settings per mode

The audio formats are the one thing that silently breaks everything when wrong: the
agent hears noise and transcribes nothing, with no error anywhere.

| Setting | Board mode | PC mode | Why |
|---|---|---|---|
| User input audio format | **`ulaw_8000`** | **`pcm_16000`** | board sends 8 kHz μ-law (4× less upload, so a phone hotspot keeps up); the PC script sends 16 kHz PCM |
| Agent output audio format | `pcm_16000` | `pcm_16000` | what the board's speaker and the PC script play |
| Authentication | **off (public agent)** | off, or on + `ELEVENLABS_API_KEY` | the board connects with only the agent ID |
| First message | **empty** | empty | the board reconnects now and then; a greeting would play each time |
| Turn eagerness | `eager` | `eager` | answers sooner |
| Speculative turn | on | on | lower latency |
| LLM | a fast one, e.g. `claude-haiku-4-5` or `gemini-2.5-flash` | same | time to first word matters more than depth |
| TTS model | `eleven_v4_turbo` (or the fastest offered) | same | latency |
| `skip_turn` system tool | **on** when activation is "say the name" | same | lets the agent stay silent when not addressed |
| Background voice detection | on in noisy rooms | off with a headset | filters other people talking. Note: it also filters voices played from a laptop speaker, so `say`-style tests stop working |

Public agent = anyone with the agent ID can talk to it on the owner's minutes. Tell the
user, and remind them never to commit the ID to a public repo (the generated project
keeps it in a git-ignored `voice_secrets.h`).

## 2. The prompt template

Fill in `{NAME}`, `{PURPOSE}` and pick ONE activation block. Keep the "how you talk"
rules: voice answers must be short, and lists or markdown sound terrible spoken.

```
# Who you are
You are {NAME}, {PURPOSE}. You live in a small device and talk out loud.

{ACTIVATION BLOCK}

# How you talk
- One or two short sentences. Simple, everyday words.
- No lists, no markdown, no emojis: everything you write is spoken aloud.
- If you don't know something, say so simply.
```

Activation block for **"say the name"** (the mic is always on):

```
# When to answer
The microphone is always on, so you also hear TV and other people.
- If the message contains your name, "{NAME}" (speech-to-text may spell it a little
  differently, e.g. {NAME_VARIANTS}), ALWAYS answer the part said to you, even if
  there is other chatter around it.
- After they call you, keep answering follow-ups for the next few turns without
  needing your name again. The conversation ends when they say goodbye or clearly
  talk to someone else.
- When your name is NOT in the message and you are NOT in a conversation, stay
  silent: use the skip_turn tool. Never say "I didn't hear my name".
```

Why so firm: an early version said "only answer when called" and the agent skipped a
clear "Hey Mira, how are you?" because it judged the speaker "mid-conversation with
someone else". Saying the name must always win.

Activation block for **"always listening"** (answers everything):

```
# When to answer
Answer whatever is said to you. If what you hear is clearly not meant for you
(TV, people talking to each other), stay silent with the skip_turn tool.
```

Activation block for **"push a button"**: none needed. The mic only streams while the
button is held, so everything the agent hears is meant for it.

`{NAME_VARIANTS}`: list 3-5 ways speech-to-text might spell the name (Mira → Mirra,
Meera, Myra, Mia). Unusual names get misheard a lot; this list fixes most of it.

## 3. Path A: with the ElevenLabs connector (MCP)

Check whether tools like `agents_create` and `agents_update_speech_recognition` are
available (search the tool list for "agents_create"). If yes:

1. `agents_create` with `name`, `prompt` (from the template), `first_message: ""`,
   `language`, and a `voice_id`. To pick a voice, search with `creative_list_voices`
   (e.g. `gender: "female"`, `use_cases: ["conversational"]`). Library voices can fail
   with `voice_not_found` until the user adds them to their account; try another, or
   ask the user to click "Add to my voices" in the ElevenLabs Voice Library.
2. `agents_update_speech_recognition`: `asr.user_input_audio_format` = `ulaw_8000`
   (board) or `pcm_16000` (pc); `vad.background_voice_detection` as in the table.
3. `agents_update_voice`: `agent_output_audio_format: "pcm_16000"`, `model_id`.
4. `agents_update_turn_taking`: `turn_eagerness: "eager"`, `speculative_turn: true`.
5. `agents_update_prompt_settings`: `llm` (fast model).
6. `agents_update_skip_turn_tool` (name and activation "say the name" or "always"):
   `name: "skip_turn"`, `params: {"system_tool_type": "skip_turn"}`.
Send these updates one at a time: several at once can conflict and return a server
   error (just retry the one that failed).

7. Confirm with `agents_get` (fields: `conversation_config.asr`,
   `conversation_config.tts.agent_output_audio_format`) that the formats stuck.
   Authentication is off by default for new agents; if the agent was made private
   (`platform_settings.auth`), the board can't connect.

The agent ID (`agent_...`) comes back from `agents_create`.

**No connector?** Offer to connect it, it saves the user all the clicking: in Claude,
open Settings → Connectors (or Customize → Connectors), add **ElevenLabs**, sign in,
then start a new conversation. In Claude Code, `/mcp` shows connected servers.
If the user prefers not to, use Path B.

## 4. Path B: by hand in the dashboard

Give the user these steps one at a time, and wait for each to be done. ElevenLabs
moves menus around now and then; the setting names stay the same, so if a menu looks
different, search the page for the setting name.

1. Go to **elevenlabs.io** → sign in → **Agents** (ElevenLabs Agents / Conversational AI).
2. **Create agent** → **Blank agent**. Name it.
3. **Agent tab**: paste the prompt from step 2 into **System prompt**. Leave **First
   message** empty. Set **Language**. Pick a fast **LLM**.
4. **Voice tab**: pick a voice. Set **TTS output format** to **PCM 16000 Hz**.
5. **Advanced tab**: set **User input audio format** to **μ-law 8000 Hz** (board mode)
   or **PCM 16000 Hz** (PC mode). Set **Turn eagerness** to **Eager**.
6. **Tools** (inside the Agent tab): under **System tools**, turn on **Skip turn**
   (activation "say the name" or "always").
7. **Security tab**: make sure **Enable authentication** is **off**.
8. **Save**. Copy the **agent ID** (starts with `agent_`), shown under the name or in
   the **Widget** tab, and paste it back here.

## 5. Checking it

- In the dashboard, the agent's **Test AI agent** button lets the user talk to it from
  the browser. Do this before touching hardware: if it fails there, it's the agent.
- After the device has talked to it, **Call history** (Conversations) shows what the
  agent heard. A conversation with **0 messages** means audio arrived but nothing was
  understood: almost always a wrong input audio format, or a silent mic.
