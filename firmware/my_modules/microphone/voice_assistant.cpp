/**
 * @file voice_assistant.cpp
 * @brief Voice AI conversation module implementation
 *
 * Threading model:
 *   - Mic capture task (Core 0) pushes samples into the mic ring buffer
 *   - Main loop (update()) drains the mic ring and sends WS binary frames —
 *     WebSocketsClient is not thread-safe, so all sends happen here
 *   - Agent audio arrives in the WS loop context (also main loop) and is
 *     enqueued into the StreamingAudioPlayer — never blocking the main loop.
 *     The player's own task prebuffers and paces speaker output, absorbing
 *     WiFi jitter and provider burstiness even when the sketch loop() is slow
 */

#include <esp_heap_caps.h>
#include "voice_assistant.h"
#include "voice_config.h"
#include "microphone.h"
#include "elevenlabs_link.h"
#include <Arduino.h>
#include <string.h>

static void micAudioTrampoline(const int16_t* samples, int count, void* ctx) {
    ((VoiceAssistant*)ctx)->_onMicAudio(samples, count);
}

VoiceAssistant::VoiceAssistant(Microphone* mic, Speaker* speaker, ElevenLabsLink* wifi)
    : _mic(mic)
    , _wifi(wifi)
    , _ringHead(0)
    , _ringTail(0)
    , _player(speaker)
    , _talking(false)
    , _handsFree(false)
    , _vadThreshold(DEFAULT_VAD_THRESHOLD)
    , _sessionActive(false)
    , _connecting(false)
    , _agentSpeaking(false)
    , _pendingSessionOpen(false)
    , _pendingSessionClose(false)
    , _pendingRingDrain(false)
    , _nextSilenceMs(0)
    , _lastActivityMs(0)
{
    _ring = (int16_t*)heap_caps_calloc(RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!_ring) _ring = (int16_t*)calloc(RING_SAMPLES, sizeof(int16_t));
}

void VoiceAssistant::begin() {
    if (_mic) {
        _mic->onAudio(micAudioTrampoline, this);
    }
    _player.begin();
}

// ========== Session control ==========

// NOTE: startTalking/stopTalking/endSession may be called from any task
// (button handlers often live in their own FreeRTOS task). WebSocketsClient
// is NOT thread-safe, so these only set flags — update() (which runs on the
// same task as wifi.maintain()) performs the actual WS sends.

void VoiceAssistant::startTalking() {
    if (!_wifi || !_wifi->isWebSocketConnected()) {
        Serial.println("[Voice] Cannot talk — gateway not connected");
        return;
    }
    if (!_sessionActive && !_connecting) {
        _connecting = true;
        _pendingSessionOpen = true;
    }
    // Drop any stale mic audio captured before the button press. Deferred to
    // update() because _ringTail is the consumer's index and this call may come
    // from another task; the cost is that samples captured in the gap are
    // dropped too, which is a few ms of pre-speech at worst.
    _pendingRingDrain = true;
    // Start the idle clock here, not in update(): a button task could press and
    // release between two update() calls, and a stale timestamp from an earlier
    // session would then hang up immediately.
    _lastActivityMs = millis();
    _talking = true;
}

void VoiceAssistant::stopTalking() {
    // update() flushes the remaining ring content, then the continuous
    // silence stream takes over so the AI's turn detection can fire. The idle
    // clock in update() closes the session if the conversation stops here.
    _talking = false;
}

void VoiceAssistant::endSession() {
    _talking = false;
    if (_sessionActive || _connecting) {
        _pendingSessionClose = true;
    }
    _sessionActive = false;
    _connecting = false;
    _agentSpeaking = false;
    _player.flush();
}

void VoiceAssistant::setHandsFree(bool on, int16_t threshold) {
    _vadThreshold = threshold;
    _handsFree = on;
    if (!on) _talking = false;
}

bool VoiceAssistant::isHandsFree() { return _handsFree; }

void VoiceAssistant::_resetSessionState() {
    _talking = false;
    _sessionActive = false;
    _connecting = false;
    _agentSpeaking = false;
    _pendingSessionOpen = false;
    _pendingSessionClose = false;
    _pendingRingDrain = false;
    _ringDrain();
    _player.flush();
    _lastActivityMs = millis();   // reopen cooldown starts now
}

void VoiceAssistant::handleVoiceState(const char* value) {
    if (!value) return;
    Serial.printf("[Voice] State: %s\n", value);

    if (strcmp(value, "connected") == 0) {
        _sessionActive = true;
        _connecting = false;
    } else if (strcmp(value, "speaking") == 0) {
        _agentSpeaking = true;
    } else if (strcmp(value, "listening") == 0) {
        _agentSpeaking = false;
        // "listening" arrives after all of the turn's audio frames (same
        // ordered stream) — let a sub-prebuffer tail play out.
        _player.endStream();
    } else if (strcmp(value, "ended") == 0) {
        _resetSessionState();
    } else if (strcmp(value, "error") == 0) {
        // Same teardown as "ended", but a different cause: the gateway could
        // not reach or authenticate with the AI provider (project not
        // provisioned, agent missing, key rejected, session refused). Pressing
        // the button again won't help, so don't let it read as a normal hangup.
        Serial.println("[Voice] Session FAILED — gateway could not start the AI session "
                       "(check the project's voice provisioning / agent config)");
        _resetSessionState();
    }
}

// ========== Audio out (agent -> speaker) ==========

void VoiceAssistant::handleAudio(const uint8_t* data, size_t length) {
    // Never blocks the main loop. The gateway paces itself to stay only
    // ~0.6 s ahead of realtime, so the player's ~1 s buffer cannot overflow
    // in normal operation.
    // Agent speech arrives quiet for a small speaker: apply VOICE_SPEAKER_GAIN
    // with saturation. The buffer may be unaligned, so go through memcpy.
    static int16_t boosted[2048];
    size_t n = length / 2;
    for (size_t off = 0; off < n; off += 2048) {
        size_t m = n - off < 2048 ? n - off : 2048;
        for (size_t i = 0; i < m; i++) {
            int16_t s;
            memcpy(&s, data + (off + i) * 2, 2);
            int32_t v = (int32_t)(s * VOICE_SPEAKER_GAIN);
            boosted[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        _player.enqueue((const uint8_t*)boosted, m * 2);
    }
}

// ========== Audio in (mic -> gateway) ==========

void VoiceAssistant::_onMicAudio(const int16_t* samples, int count) {
    // Capture-task context: copy into the ring, drop if full (never block).
    // Hands-free keeps capturing between sessions so update() has pre-roll.
    if (!_talking && !_handsFree) return;
    int head = _ringHead.load(std::memory_order_relaxed);
    int tail = _ringTail.load(std::memory_order_acquire);
    for (int i = 0; i < count; i++) {
        int next = (head + 1) % RING_SAMPLES;
        if (next == tail) break;  // ring full — drop the rest
        _ring[head] = samples[i];
        head = next;
    }
    // Release: sample stores above must be visible before the index publish
    _ringHead.store(head, std::memory_order_release);
}

void VoiceAssistant::update() {
    if (!_wifi) return;

    if (!_wifi->isWebSocketConnected()) {
        // Connection dropped. The gateway tears the session down on its side
        // (its "ended" message can't reach us), so clear all session state —
        // otherwise the next startTalking() would skip voice_start and stream
        // into a void forever.
        if (_sessionActive || _connecting || _talking || _pendingSessionOpen) {
            Serial.println("[Voice] Gateway connection lost — resetting voice session");
            _resetSessionState();
        }
        return;
    }

    // Idle auto-hang-up. Nothing else stops the session: after the button is
    // released the device streams silence forever (see the silence branch
    // below) and the gateway relays every frame to the billed provider, so a
    // ~15 s exchange used to bill for the gateway's full ~2 min watchdog.
    //
    // The clock is held back by mic audio, by agent speech AND by audio still
    // in the jitter buffer — deliberately not a timer armed by stopTalking().
    // The agent typically starts replying 1-2 s after the button is released,
    // so a plain timer would cut the answer off mid-sentence.
    uint32_t nowMs = millis();
    bool voiced = _handsFree && _mic && _mic->getSoundLevel() > _vadThreshold;

    // Hands-free wake: no session yet, so only keep the newest pre-roll until
    // the mic hears voice, then open a session and keep the pre-roll so the
    // first word reaches the AI. _talking stays on for the whole session.
    // The cooldown lets the gateway's "ended" for the last session arrive
    // first, so it can't tear down the session we are about to open.
    if (_handsFree && !_sessionActive && !_connecting && !_pendingSessionClose) {
        if ((!voiced && !VOICE_ALWAYS_ON) || nowMs - _lastActivityMs < REOPEN_COOLDOWN_MS) {
            _ringTrim(PREROLL_SAMPLES);
            return;
        }
        Serial.println("[Voice] Voice detected — opening hands-free session");
        _connecting = true;
        _pendingSessionOpen = true;
        _talking = true;
        _lastActivityMs = nowMs;
    }

    // Push-to-talk counts the held button as activity; hands-free streams the
    // whole session, so only actual voice keeps it alive.
    bool active = _handsFree ? voiced : _talking;
    if (active || _agentSpeaking || _player.bufferedSamples() > 0) {
        _lastActivityMs = nowMs;
    } else if (!VOICE_ALWAYS_ON && (_sessionActive || _connecting) &&
               nowMs - _lastActivityMs > IDLE_SESSION_END_MS) {
        Serial.printf("[Voice] Idle for %us — ending session\n",
                      (unsigned)(IDLE_SESSION_END_MS / 1000));
        endSession();  // arms _pendingSessionClose, sent just below
        _lastActivityMs = nowMs;  // starts the hands-free reopen cooldown
    }

    // Deferred drain requested by startTalking() — _ringTail is ours to write
    if (_pendingRingDrain) {
        _pendingRingDrain = false;
        _ringDrain();
    }

    // Deferred session control — WS sends happen only on this task
    if (_pendingSessionOpen) {
        _pendingSessionOpen = false;
        Serial.println("[Voice] Opening voice session...");
        _wifi->sendControl("voice_start");
    }
    if (_pendingSessionClose) {
        _pendingSessionClose = false;
        _wifi->sendControl("voice_stop");
    }

    // Mute while the agent speaks — and while buffered reply audio is still
    // playing out (the gateway's "listening" arrives up to ~1 s before the
    // jitter buffer drains). The mic would mostly pick up the speaker
    // (no echo cancellation on this hardware), confusing the AI.
    if (_agentSpeaking || _player.bufferedSamples() > 0) _muteUntilMs = nowMs + 400;
    if (_agentSpeaking || _player.bufferedSamples() > 0 || (int32_t)(nowMs - _muteUntilMs) < 0) {
        _ringDrain();
        // The WS library delivers at most one frame per maintain() call, so a
        // slow sketch loop() would throttle agent audio below realtime. Pump
        // extra calls here — handleAudio only enqueues, so each one is cheap.
        for (int i = 0; i < 6 && _agentSpeaking && _wifi->isWebSocketConnected(); i++) {
            _wifi->maintain();
        }
        return;
    }

    // Up to 3 frames per loop keeps the loop responsive while draining
    // faster than realtime (3 x 60 ms of audio per iteration).
    for (int burst = 0; burst < 3; burst++) {
        int available = _ringAvailable();
        if (_talking) {
            if (available < FRAME_SAMPLES) break;
            _ringPop(_frame, FRAME_SAMPLES);
            if (!_sendFrame(_frame, FRAME_SAMPLES)) return;
        } else if (available > 0) {
            // Button released: flush the tail of the utterance (including the
            // final partial frame).
            int n = _ringPop(_frame, FRAME_SAMPLES);
            if (!_sendFrame(_frame, n)) return;
        } else if (_sessionActive || _connecting) {
            // Continuous silence stream, paced at realtime. A real microphone
            // never stops sending; the AI's turn detection needs ongoing
            // silence to decide the user finished talking — if the stream
            // just stops, the agent never replies. Costs ~256 kbps only while
            // a session is open; the gateway ends idle sessions since pure
            // silence doesn't count as activity there.
            uint32_t now = millis();
            if ((int32_t)(now - _nextSilenceMs) < 0) break;  // not due yet
            if (now - _nextSilenceMs > 300) _nextSilenceMs = now;  // resync after gaps
            memset(_frame, 0, FRAME_SAMPLES * sizeof(int16_t));
            if (!_sendFrame(_frame, FRAME_SAMPLES)) return;
            _nextSilenceMs += FRAME_MS;
        } else {
            break;
        }
    }
}

int VoiceAssistant::_ringAvailable() {
    int head = _ringHead.load(std::memory_order_acquire);
    int tail = _ringTail.load(std::memory_order_relaxed);
    return head >= tail ? head - tail : head + RING_SAMPLES - tail;
}

int VoiceAssistant::_ringPop(int16_t* dest, int maxSamples) {
    int available = _ringAvailable();
    int count = available < maxSamples ? available : maxSamples;
    int tail = _ringTail.load(std::memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        dest[i] = _ring[tail];
        tail = (tail + 1) % RING_SAMPLES;
    }
    // Release: sample reads above must complete before the slots are reusable
    _ringTail.store(tail, std::memory_order_release);
    return count;
}

void VoiceAssistant::_ringDrain() {
    _ringTail.store(_ringHead.load(std::memory_order_acquire),
                    std::memory_order_release);
}

void VoiceAssistant::_ringTrim(int keepSamples) {
    int available = _ringAvailable();
    if (available <= keepSamples) return;
    int tail = _ringTail.load(std::memory_order_relaxed);
    _ringTail.store((tail + available - keepSamples) % RING_SAMPLES,
                    std::memory_order_release);
}

bool VoiceAssistant::_sendFrame(const int16_t* samples, int count) {
    return _wifi->sendBinary((const uint8_t*)samples, count * sizeof(int16_t));
}

bool VoiceAssistant::isSessionActive() { return _sessionActive; }
bool VoiceAssistant::isConnecting() { return _connecting; }
bool VoiceAssistant::isTalking() { return _talking; }
bool VoiceAssistant::isAgentSpeaking() { return _agentSpeaking; }
int VoiceAssistant::bufferedAudioSamples() { return _player.bufferedSamples(); }
