/**
 * @file voice_assistant.h
 * @brief Voice AI conversation module for Athera
 *
 * Software-only module (no dedicated hardware) that turns a Microphone +
 * Speaker + ElevenLabsLink combo into a push-to-talk AI voice assistant.
 * Mic audio streams to the Atech gateway as binary WebSocket frames
 * (raw PCM16 mono @ 16 kHz); the gateway bridges the conversation to an
 * AI voice agent and streams reply audio back the same way.
 *
 * Usage (typical generated wiring):
 *   Microphone mic(...); Speaker speaker(...); ElevenLabsLink wifi(...);
 *   VoiceAssistant voice(&mic, &speaker, &wifi);
 *
 *   void setup() {
 *     mic.begin(); speaker.begin(); wifi.connect();
 *     voice.begin();
 *     wifi.onBinary([](const uint8_t* data, size_t len) { voice.handleAudio(data, len); });
 *     wifi.onInternalMessage([](const char* action, const char* value) {
 *       if (strcmp(action, "voice_state") == 0) { voice.handleVoiceState(value); return true; }
 *       return false;
 *     });
 *   }
 *
 *   void loop() {
 *     wifi.maintain();
 *     voice.update();                          // REQUIRED every loop
 *     if (button.wasPressed())  voice.startTalking();   // push-to-talk
 *     if (button.wasReleased()) voice.stopTalking();
 *   }
 *
 * Conversation model (half-duplex, push-to-talk):
 *   - Hold button -> mic streams to the AI (first press also opens the session)
 *   - Release button -> the module keeps streaming SILENCE frames, paced at
 *     realtime. This is required: the AI's turn detection needs ongoing
 *     silence (like a real microphone) to decide the user finished talking —
 *     if the stream just stops, the agent never replies.
 *   - Agent reply plays through the speaker; mic is muted while agent speaks
 *   - Session closes on endSession(), automatically after
 *     IDLE_SESSION_END_MS with nobody talking (see below), or — as a
 *     backstop, not the primary guard — when the gateway times it out after
 *     ~2 minutes without voiced audio (the silence stream doesn't count)
 *
 * Hands-free mode (setHandsFree(true), no button needed):
 *   - The mic always fills the ring; while no session is open only the last
 *     PREROLL_SAMPLES are kept, so the start of the first word survives
 *   - When the mic level crosses the threshold, a session opens and the
 *     pre-roll plus live mic audio streams to the AI continuously
 *   - The session hangs up after IDLE_SESSION_END_MS without voice. Deciding
 *     whether the user actually addressed the agent (its wake name) is the
 *     agent's job: its prompt tells it to skip turns not meant for it
 *
 *   void setup() { ...; voice.begin(); voice.setHandsFree(true); }
 *   void loop()  { wifi.maintain(); voice.update(); }
 */

#ifndef VOICE_ASSISTANT_MODULE_H
#define VOICE_ASSISTANT_MODULE_H

#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include "streaming_audio_player.h"

class Microphone;
class Speaker;
class ElevenLabsLink;

class VoiceAssistant {
public:
    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int FRAME_SAMPLES = 960;                 // 60 ms per WS frame
    static constexpr int FRAME_MS = FRAME_SAMPLES * 1000 / SAMPLE_RATE;
    static constexpr int RING_SAMPLES = 4096;                 // ~250 ms mic buffer (RAM is tight: TLS needs ~50 KB)
    static constexpr int PREROLL_SAMPLES = 3200;              // 200 ms kept before voice is detected
    static constexpr int16_t DEFAULT_VAD_THRESHOLD = 800;     // mic RMS (0-32767) that counts as voice
    static constexpr uint32_t REOPEN_COOLDOWN_MS = 2000;      // hands-free: wait after a hang-up

    // Auto-hang-up threshold. An open session streams silence continuously and
    // the gateway relays every frame to the (per-minute billed) AI provider, so
    // a session nobody is using costs money until something ends it. The device
    // hangs up first; the gateway's ~2 min watchdog stays as the backstop.
    // Generous enough to survive a slow reply plus a pause for thought — the
    // clock is held back by agent speech and buffered playback, not just the
    // button, so it only expires when genuinely nothing is happening.
    static constexpr uint32_t IDLE_SESSION_END_MS = 45000;

    VoiceAssistant(Microphone* mic, Speaker* speaker, ElevenLabsLink* wifi);

    /**
     * @brief Hook into the microphone's capture stream. Call after mic.begin().
     */
    void begin();

    /**
     * @brief Pump mic audio to the gateway. Call every loop() iteration.
     */
    void update();

    /**
     * @brief Start streaming mic audio (button pressed).
     * Opens the voice session on first use — the AI connection takes ~1s,
     * during which early audio is buffered server-side.
     */
    void startTalking();

    /**
     * @brief Stop streaming mic audio (button released).
     * The module continues streaming silence so the AI's turn detection
     * closes the turn and the agent replies. update() ends the session on its
     * own once nothing has happened for IDLE_SESSION_END_MS.
     */
    void stopTalking();

    /**
     * @brief End the voice session entirely (e.g. long-press).
     * Optional: update() already hangs up after IDLE_SESSION_END_MS of
     * inactivity; the gateway's ~2 min timeout only covers a device that
     * can't hang up itself (wedged, or the WS dropped mid-session).
     */
    void endSession();

    /**
     * @brief Always-on listening instead of push-to-talk.
     * @param on        true = open a session whenever the mic hears voice
     * @param threshold mic RMS level (0-32767) that counts as voice. Raise it
     *                  in a noisy room; watch mic levels in the monitor to tune.
     */
    void setHandsFree(bool on, int16_t threshold = DEFAULT_VAD_THRESHOLD);
    bool isHandsFree();

    /**
     * @brief Feed a "voice_state" message value from the gateway.
     * Wire from the wifi.onMessage handler for action == "voice_state".
     * Values: connected | listening | speaking | ended | error
     */
    void handleVoiceState(const char* value);

    /**
     * @brief Queue an agent audio frame for playback (raw PCM16 mono @ 16 kHz).
     * Wire from wifi.onBinary. Non-blocking: audio goes into the
     * StreamingAudioPlayer's jitter buffer, so network hiccups and a slow
     * sketch loop() don't cause gaps in the speaker output.
     */
    void handleAudio(const uint8_t* data, size_t length);

    bool isSessionActive();   // true between "connected" and "ended"/"error"
    bool isConnecting();      // true after startTalking() until "connected"
    bool isTalking();         // true while streaming mic audio
    bool isAgentSpeaking();   // true while the agent's reply is playing
    int bufferedAudioSamples();  // reply audio waiting in the playback buffer

    // Mic capture callback (public for static trampoline, do not call directly)
    void _onMicAudio(const int16_t* samples, int count);

private:
    Microphone* _mic;
    ElevenLabsLink* _wifi;

    // Single-producer (capture task, core 0) / single-consumer (main loop)
    // ring buffer. Atomics with release/acquire ordering: the sample stores
    // must be visible before the index publish crosses cores.
    int16_t* _ring;   // RING_SAMPLES, in PSRAM so internal RAM stays free for TLS
    std::atomic<int> _ringHead;   // written by capture task
    std::atomic<int> _ringTail;   // written by main loop
    int16_t _frame[FRAME_SAMPLES];

    // Agent audio playback — jitter-buffered so network hiccups and a slow
    // sketch loop() don't cause gaps in the speaker output.
    StreamingAudioPlayer _player;

    volatile bool _talking;
    volatile bool _handsFree;
    int16_t _vadThreshold;
    volatile bool _sessionActive;
    volatile bool _connecting;
    volatile bool _agentSpeaking;
    // WS control frames are sent only from update() (main loop) — flags defer
    // startTalking()/endSession() requests made from other tasks.
    volatile bool _pendingSessionOpen;
    volatile bool _pendingSessionClose;
    // _ringTail belongs to the consumer (update()), so startTalking()'s drain
    // is deferred here too rather than written from the calling task.
    volatile bool _pendingRingDrain;
    uint32_t _muteUntilMs = 0;  // keep the mic muted briefly after reply audio (speaker echo)
    uint32_t _nextSilenceMs;  // pacing clock for the continuous silence stream
    volatile uint32_t _lastActivityMs;  // idle clock for the auto-hang-up

    int _ringAvailable();
    int _ringPop(int16_t* dest, int maxSamples);
    void _ringDrain();
    void _ringTrim(int keepSamples);
    void _resetSessionState();
    bool _sendFrame(const int16_t* samples, int count);
};

#endif // VOICE_ASSISTANT_MODULE_H
