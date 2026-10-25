// Voice settings, compiled into the firmware by `atech upload`.
// The board talks to ElevenLabs directly, so only a public agent ID is
// needed. The setup script rewrites the values below; edit them to tune.
#pragma once

// WiFi and the ElevenLabs agent ID live in voice_secrets.h (git-ignored).
// Copy voice_secrets.example.h to voice_secrets.h and fill it in.
#if __has_include("voice_secrets.h")
#include "voice_secrets.h"
#else
#error "Copy voice_secrets.example.h to voice_secrets.h and add your WiFi + agent ID"
#endif

// Mic RMS level (0-32767) that counts as someone speaking. Raise it if the
// room noise keeps opening sessions; lower it if quiet speech gets missed.
#define VOICE_VAD_THRESHOLD  1200

// Heartbeat the main loop stamps; a watchdog task restarts the board if it stops.
static volatile uint32_t loopBeatMs = 0;

// 1 = listen all the time (the mic decides when to talk to the agent).
// 0 = push-to-talk: the project's code calls voice.startTalking() /
//     voice.stopTalking() (for example from a button).
#define VOICE_HANDS_FREE      1

// 1 = keep one session open all the time (opens at boot, reconnects on drop):
// no connect delay, first words never lost. Bills ElevenLabs minutes while on.
// 0 = open a session only when the mic hears voice, hang up after 45 s idle.
#define VOICE_ALWAYS_ON       1

// Loudness of the agent's voice: gain on its audio (clipped safely) and the
// speaker volume (0.0-1.0). Turn the gain down if she sounds distorted.
#define VOICE_SPEAKER_GAIN    2.5f
#define VOICE_SPEAKER_VOLUME  0.9f
