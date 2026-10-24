/**
 * @file elevenlabs_link.h
 * @brief Direct WiFi link from the board to an ElevenLabs agent
 *
 * Drop-in for the AtechWiFi gateway connection that VoiceAssistant uses, but
 * talks to ElevenLabs Conversational AI directly, so no Atech project ID or
 * device token is needed — only a public agent ID.
 *
 *   wss://api.elevenlabs.io/v1/convai/conversation?agent_id=<id>
 *   Send: {"user_audio_chunk": "<base64 PCM16 mono 16 kHz>"}
 *   Recv: conversation_initiation_metadata | audio | interruption | ping | ...
 *
 * VoiceAssistant's control words map onto it: "voice_start" opens the
 * conversation WebSocket, "voice_stop" closes it, and the link reports
 * voice_state connected / speaking / listening / ended back, the same way
 * the gateway did.
 */

#ifndef ELEVENLABS_LINK_H
#define ELEVENLABS_LINK_H

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>

class ElevenLabsLink {
public:
    typedef void (*BinaryCallback)(const uint8_t* data, size_t length);
    typedef bool (*InternalMessageCallback)(const char* action, const char* value);

    // ElevenLabs sends no "agent finished" event, so the reply counts as done
    // once no audio has arrived for this long.
    static constexpr uint32_t SPEAKING_TAIL_MS = 1000;
    // Mic audio captured while the TLS handshake runs (~1 s) is held here and
    // sent once the conversation opens, so the first word isn't lost.
    static constexpr size_t PENDING_BYTES = 8000;    // 250 ms of PCM16 @ 16 kHz (the always-on session rarely needs it)

    ElevenLabsLink(const char* ssid, const char* password, const char* agentId)
        : _ssid(ssid), _password(password), _agentId(agentId) {}

    bool connect() {
        WiFi.mode(WIFI_STA);
        WiFi.begin(_ssid, _password);
        Serial.printf("[ElevenLabs] WiFi: connecting to %s", _ssid);
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
            delay(250);
            Serial.print(".");
        }
        Serial.println();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[ElevenLabs] WiFi failed — will keep retrying in the background");
            return false;
        }
        Serial.printf("[ElevenLabs] WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    // VoiceAssistant only opens sessions while this is true.
    bool isWebSocketConnected() { return WiFi.status() == WL_CONNECTED; }

    void onBinary(BinaryCallback cb) { _binaryCallback = cb; }
    void onInternalMessage(InternalMessageCallback cb) { _internalCallback = cb; }

    // Backpressure: ElevenLabs can stream a reply faster than realtime, more
    // than the ~1 s playback buffer holds. While this returns false the link
    // stops reading the socket, so TCP holds the rest on the server side.
    typedef bool (*CanReceiveCallback)();
    void onCanReceive(CanReceiveCallback cb) { _canReceive = cb; }

    void maintain() {
        uint32_t now = millis();
        if (WiFi.status() != WL_CONNECTED) {
            if (now - _lastWifiRetry > 10000) {
                _lastWifiRetry = now;
                WiFi.reconnect();
            }
            if (_sessionOpen) _closeSession(true);
            return;
        }
        bool canRx = !_canReceive || _canReceive();
        if (_sessionOpen && canRx) _ws.loop();
        // Not reading because our own playback buffer is full is not the end of
        // the reply: hold the tail clock, or the mic unmutes mid-sentence and the
        // agent hears itself and stops.
        now = millis();   // _ws.loop() may have just stamped _lastAudioMs later than `now`
        if (_speaking && !canRx) _lastAudioMs = now;
        if (_speaking && (int32_t)(now - _lastAudioMs) > (int32_t)SPEAKING_TAIL_MS) {
            _speaking = false;
            _state("listening");
        }
    }

    bool sendControl(const char* type) {
        if (strcmp(type, "voice_start") == 0) {
            _openSession();
            return true;
        }
        if (strcmp(type, "voice_stop") == 0) {
            _closeSession(false);
            return true;
        }
        return false;
    }

    bool sendBinary(const uint8_t* data, size_t length) {
        if (!_sessionOpen) return false;
        if (!_conversationReady) {
            size_t room = PENDING_BYTES - _pendingLen;
            size_t n = length < room ? length : room;
            memcpy(_pending + _pendingLen, data, n);
            _pendingLen += n;
            return true;
        }
        return _sendAudio(data, length);
    }

private:
    const char* _ssid;
    const char* _password;
    const char* _agentId;
    WebSocketsClient _ws;
    BinaryCallback _binaryCallback = nullptr;
    InternalMessageCallback _internalCallback = nullptr;
    CanReceiveCallback _canReceive = nullptr;

    bool _sessionOpen = false;
    bool _handlerRegistered = false;
    bool _conversationReady = false;
    bool _speaking = false;
    uint32_t _lastAudioMs = 0;
    uint32_t _lastWifiRetry = 0;
    unsigned long _chunksSent = 0;
    uint8_t* _pending = _allocPending();   // PSRAM so internal RAM stays free for TLS
    size_t _pendingLen = 0;
    char _txBuf[3200];   // {"user_audio_chunk":"..."} for one 60 ms frame

    static uint8_t* _allocPending() {
        uint8_t* p = (uint8_t*)heap_caps_malloc(PENDING_BYTES, MALLOC_CAP_SPIRAM);
        return p ? p : (uint8_t*)malloc(PENDING_BYTES);
    }

    void _state(const char* value) {
        if (_internalCallback) _internalCallback("voice_state", value);
    }

    void _openSession() {
        if (_sessionOpen) return;
        _ws.disconnect();   // free the TLS client a dropped connection left behind
        String path = String("/v1/convai/conversation?agent_id=") + _agentId;
        _pendingLen = 0;
        _conversationReady = false;
        _speaking = false;
        _sessionOpen = true;
        // Register once: re-registering stacks handlers, so every frame would be
        // handled (and every audio chunk queued) once per past session.
        if (!_handlerRegistered) {
            _ws.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
                _onEvent(type, payload, length);
            });
            _handlerRegistered = true;
        }
        _ws.setReconnectInterval(0);
        _ws.beginSSL("api.elevenlabs.io", 443, path.c_str());
        Serial.printf("[ElevenLabs] Opening conversation... free heap %u, largest block %u, PSRAM free %u\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());
    }

    void _closeSession(bool notify) {
        if (!_sessionOpen) return;
        _sessionOpen = false;
        _conversationReady = false;
        _speaking = false;
        _ws.disconnect();
        Serial.println("[ElevenLabs] Conversation closed");
        if (notify) _state("ended");
    }

    // G.711 mu-law encode of one 16-bit sample.
    static uint8_t _ulaw(int16_t pcm) {
        const int BIAS = 0x84, CLIP = 32635;
        int sign = (pcm >> 8) & 0x80;
        int s = sign ? -(int)pcm : pcm;
        if (s > CLIP) s = CLIP;
        s += BIAS;
        int exp = 7;
        for (int mask = 0x4000; (s & mask) == 0 && exp > 0; mask >>= 1) exp--;
        int mant = (s >> (exp + 3)) & 0x0F;
        return (uint8_t)~(sign | (exp << 4) | mant);
    }

    bool _sendAudio(const uint8_t* data, size_t length) {
        // The agent takes ulaw_8000: 1/4 of the bytes of 16 kHz PCM, which a
        // phone hotspot uplink keeps up with (16 kHz PCM stalled sends for
        // seconds). Average sample pairs to 8 kHz, then mu-law encode.
        static uint8_t ulaw[1024];
        size_t pairs = length / 4;
        if (pairs > sizeof(ulaw)) pairs = sizeof(ulaw);
        for (size_t i = 0; i < pairs; i++) {
            int16_t a, b;
            memcpy(&a, data + i * 4, 2);
            memcpy(&b, data + i * 4 + 2, 2);
            ulaw[i] = _ulaw((int16_t)(((int32_t)a + b) / 2));
        }
        data = ulaw;
        length = pairs;
        if (length == 0) return true;

        static const char head[] = "{\"user_audio_chunk\":\"";
        size_t pos = sizeof(head) - 1;
        memcpy(_txBuf, head, pos);
        size_t written = 0;
        // Leave room for the closing `"}` and the terminator
        if (mbedtls_base64_encode((unsigned char*)_txBuf + pos, sizeof(_txBuf) - pos - 3,
                                  &written, data, length) != 0) {
            return false;
        }
        pos += written;
        _txBuf[pos++] = '"';
        _txBuf[pos++] = '}';
        _txBuf[pos] = '\0';
        _chunksSent++;
        bool ok = _ws.sendTXT(_txBuf, pos);
        if (!ok || _chunksSent % 25 == 0)
            Serial.printf("[ElevenLabs] t=%lu chunk %lu %s, heap %u\n", millis(), _chunksSent, ok ? "ok" : "SEND FAILED", ESP.getFreeHeap());
        return ok;
    }

    void _flushPending() {
        const size_t frame = 1920;   // 60 ms, the same size VoiceAssistant sends
        for (size_t off = 0; off < _pendingLen; off += frame) {
            size_t n = _pendingLen - off < frame ? _pendingLen - off : frame;
            _sendAudio(_pending + off, n);
        }
        _pendingLen = 0;
    }

    // Returns a pointer to the first char of a JSON string value, or nullptr.
    static char* _findString(char* json, const char* key, size_t* len) {
        char* p = strstr(json, key);
        if (!p) return nullptr;
        p += strlen(key);
        while (*p == ' ' || *p == ':') p++;
        if (*p != '"') return nullptr;
        char* end = strchr(++p, '"');
        if (!end) return nullptr;
        *len = end - p;
        return p;
    }

    void _onEvent(WStype_t type, uint8_t* payload, size_t length) {
        switch (type) {
            case WStype_CONNECTED:
                Serial.println("[ElevenLabs] WebSocket connected");
                break;
            case WStype_DISCONNECTED:
                if (_sessionOpen) {
                    Serial.printf("[ElevenLabs] t=%lu Disconnected by server (%u B: %.*s), sent %lu chunks\n",
                                  millis(), (unsigned)length, (int)length, (const char*)payload, _chunksSent);
                    if (!_conversationReady) {
                        // Never got connected: after a session the heap is too
                        // fragmented for another TLS handshake. A reboot gives a
                        // clean heap and Mira is back in ~10 s.
                        Serial.printf("[ElevenLabs] Reconnect failed (largest block %u) - restarting the board\n", ESP.getMaxAllocHeap());
                        delay(50);
                        ESP.restart();
                    }
                    _sessionOpen = false;
                    _conversationReady = false;
                    _speaking = false;
                    _state("ended");
                }
                break;
            case WStype_TEXT:
                _onText((char*)payload, length);
                break;
            case WStype_ERROR:
                Serial.printf("[ElevenLabs] WebSocket error (%u B: %.*s)\n",
                              (unsigned)length, (int)length, (const char*)payload);
                break;
            default:
                break;
        }
    }

    void _onText(char* json, size_t length) {
        // The library NUL-terminates text payloads, so plain C string search is safe.
        size_t len = 0;
        char* type = _findString(json, "\"type\"", &len);
        if (!type) {
            Serial.printf("[ElevenLabs] (untyped %u B) %.120s\n", (unsigned)length, json);
            return;
        }
        Serial.printf("[ElevenLabs] t=%lu <- %.*s (%u B)\n", millis(), (int)len, type, (unsigned)length);

        if (len == 5 && strncmp(type, "audio", 5) == 0) {
            char* b64 = _findString(json, "\"audio_base_64\"", &len);
            if (!b64 || !len) return;
            // Decode in place: base64 always shrinks, so the output fits
            size_t outLen = 0;
            if (mbedtls_base64_decode((unsigned char*)b64, len, &outLen,
                                      (const unsigned char*)b64, len) != 0) return;
            if (!_speaking) {
                _speaking = true;
                _state("speaking");
            }
            _lastAudioMs = millis();
            if (_binaryCallback) _binaryCallback((const uint8_t*)b64, outLen);
        } else if (len == 4 && strncmp(type, "ping", 4) == 0) {
            char* id = strstr(json, "\"event_id\"");
            if (!id) return;
            int eventId = atoi(strchr(id, ':') + 1);
            char pong[64];
            int n = snprintf(pong, sizeof(pong), "{\"type\":\"pong\",\"event_id\":%d}", eventId);
            _ws.sendTXT(pong, n);
        } else if (strncmp(type, "conversation_initiation_metadata", len) == 0) {
            Serial.printf("[ElevenLabs] Conversation ready, free heap %u, min ever %u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
            _conversationReady = true;
            _flushPending();
            _state("connected");
        } else if (strncmp(type, "interruption", len) == 0) {
            _speaking = false;
            _state("listening");
        } else if (strncmp(type, "user_transcript", len) == 0) {
            size_t tl = 0;
            char* t = _findString(json, "\"user_transcript\":", &tl);
            if (t) Serial.printf("[ElevenLabs] You: %.*s\n", (int)tl, t);
        } else if (strncmp(type, "agent_response", len) == 0 && len == 14) {
            size_t tl = 0;
            char* t = _findString(json, "\"agent_response\":", &tl);
            if (t) Serial.printf("[ElevenLabs] Mira: %.*s\n", (int)tl, t);
        }
    }
};

#endif // ELEVENLABS_LINK_H
