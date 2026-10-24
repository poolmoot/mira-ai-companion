/**
 * @file speaker.cpp
 * @brief Non-blocking I2S Speaker Module Implementation
 *
 * Audio playback runs in a background FreeRTOS task so the main loop
 * stays responsive. New notes interrupt any currently playing note.
 */

#include "speaker.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include <mbedtls/base64.h>

// Static trampoline for FreeRTOS task
static void audioTaskTrampoline(void* param) {
    ((Speaker*)param)->_audioTaskLoop();
}

// Auto-assign I2S peripherals (ESP32-S3 has I2S_NUM_0 and I2S_NUM_1)
int Speaker::_nextI2SPort = 0;

Speaker::Speaker(int bclkPin, int lrcPin, int doutPin)
    : _i2sPort(_nextI2SPort++)
    , _bclkPin(bclkPin)
    , _lrcPin(lrcPin)
    , _doutPin(doutPin)
    , _sampleRate(DEFAULT_SAMPLE_RATE)
    , _volume(0.4f)  // Safe default — 1.0 clips on MAX98357A; codegen still calls setVolume(0.4f)
    , _initialized(false)
    , _playing(false)
    , _stopRequested(false)
    , _liveNumFreqs(0)
    , _liveDurationMs(0)
    , _liveUpdated(false)
    , _inContinuousMode(false)
    , _noteQueue(nullptr)
    , _audioTask(nullptr)
    , _rtttlTask(nullptr)
    , _rtttlMelody(nullptr)
    , _rtttlPlaying(false)
{
}

void Speaker::begin(int sampleRate) {
    _sampleRate = sampleRate;

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = (uint32_t)_sampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = _bclkPin,
        .ws_io_num = _lrcPin,
        .data_out_num = _doutPin,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    i2s_port_t port = (i2s_port_t)_i2sPort;
    esp_err_t err = i2s_driver_install(port, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("Speaker I2S%d install failed: %s\n", _i2sPort, esp_err_to_name(err));
        return;
    }

    err = i2s_set_pin(port, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("Speaker I2S%d pin config failed: %s\n", _i2sPort, esp_err_to_name(err));
        return;
    }

    _initialized = true;

    // Note queue (FIFO, depth 16). playNote() appends to it so chained calls play
    // as a real melody — `playNote(C); playNote(E); playNote(G);` works without the
    // caller needing to wait between calls. playTone()/playChord() drain the queue
    // first so they still behave as "interrupt with new sound".
    _noteQueue = xQueueCreate(16, sizeof(NoteRequest));

    // Create background audio task on Core 0 (leaves Core 1 for main loop)
    xTaskCreatePinnedToCore(
        audioTaskTrampoline,
        "AudioTask",
        4096,
        this,
        2,       // Priority 2 (above idle, responsive to new notes)
        &_audioTask,
        0        // Core 0
    );
}

void Speaker::playTone(float freq, int durationMs) {
    if (!_initialized) return;

    // playTone is an INTERRUPTING call — replace any pending melody notes with
    // this single tone. durationMs <= 0 = play until stop().
    _liveFreqs[0] = freq;
    _liveNumFreqs = 1;
    _liveDurationMs = durationMs;
    _liveUpdated = true;

    // Always drain any future melody notes so they don't queue up behind this tone.
    xQueueReset(_noteQueue);

    if (_playing && _inContinuousMode) {
        // Mid-continuous playback (previous playTone/playChord). The audio task
        // is in _writeToneContinuous and will absorb the new freqs via
        // _liveUpdated. Do NOT enqueue — that would double-play the same tone
        // after the live-updated playback finishes.
        return;
    }

    // Either idle, or mid-playNote/RTTTL (timed _writeTone path). Force the
    // task to break out of any in-progress timed note and pick up this tone.
    NoteRequest req = {};
    req.freqs[0] = freq;
    req.numFreqs = 1;
    req.durationMs = durationMs;
    req.withGap = false;
    _stopRequested = true;
    xQueueSend(_noteQueue, &req, 0);
}

void Speaker::playNote(float freq, int durationMs) {
    if (!_initialized || !_noteQueue) return;

    // playNote APPENDS to the melody queue — chained calls play in order without
    // the caller needing to wait. To interrupt, call stop() first or use playTone().
    NoteRequest req = {};
    req.freqs[0] = freq;
    req.numFreqs = 1;
    req.durationMs = durationMs;
    req.withGap = true;
    // Non-blocking. If the queue is full (>16 pending notes — a very long
    // inline jingle) the extra note is dropped rather than stalling the caller.
    xQueueSend(_noteQueue, &req, 0);
}

void Speaker::playChord(const float* freqs, int numFreqs, int durationMs) {
    if (!_initialized || numFreqs <= 0) return;

    // playChord is an INTERRUPTING call — replace any pending melody notes.
    // durationMs <= 0 = play until stop().
    int n = min(numFreqs, (int)MAX_CHORD_NOTES);
    for (int i = 0; i < n; i++) {
        _liveFreqs[i] = freqs[i];
    }
    _liveNumFreqs = n;
    _liveDurationMs = durationMs;
    _liveUpdated = true;

    // Always drain any future melody notes.
    xQueueReset(_noteQueue);

    if (_playing && _inContinuousMode) {
        // Mid-continuous playback — live update absorbs the new freqs. No enqueue.
        return;
    }

    // Either idle, or mid-playNote/RTTTL. Force the timed path to bail and pick
    // up this chord.
    NoteRequest req = {};
    for (int i = 0; i < n; i++) {
        req.freqs[i] = freqs[i];
    }
    req.numFreqs = n;
    req.durationMs = durationMs;
    req.withGap = false;
    _stopRequested = true;
    xQueueSend(_noteQueue, &req, 0);
}

void Speaker::stop() {
    if (!_initialized) return;

    // Clear live frequencies and signal stop
    _liveNumFreqs = 0;
    _liveDurationMs = 0;
    _liveUpdated = true;
    _stopRequested = true;

    // Drain the whole queue so no pending melody notes play after this stop().
    xQueueReset(_noteQueue);
}

bool Speaker::isPlaying() {
    return _playing;
}

void Speaker::setVolume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    _volume = vol;
}

float Speaker::getVolume() {
    return _volume;
}

// ========== Background task ==========

void Speaker::_audioTaskLoop() {
    NoteRequest req;
    while (true) {
        // Wait for a note request (blocks until one arrives)
        if (xQueueReceive(_noteQueue, &req, portMAX_DELAY) == pdTRUE) {
            _stopRequested = false;
            _liveUpdated = false;
            _playing = true;

            // Check if all frequencies are silent/rest
            bool allSilent = true;
            for (int i = 0; i < req.numFreqs; i++) {
                if (req.freqs[i] > 0) { allSilent = false; break; }
            }

            if (allSilent) {
                _writeSilence(req.durationMs);
            } else if (req.withGap) {
                // playNote: 85% tone, 15% silence (used by RTTTL/melodies)
                int playMs = (int)(req.durationMs * 0.85f);
                int gapMs = req.durationMs - playMs;
                _writeTone(req.freqs, req.numFreqs, playMs);
                if (!_stopRequested && gapMs > 0) {
                    _writeSilence(gapMs);
                }
            } else {
                // Continuous playback — use live frequency mode
                // Set initial live state from the request
                for (int i = 0; i < req.numFreqs; i++) {
                    _liveFreqs[i] = req.freqs[i];
                }
                _liveNumFreqs = req.numFreqs;
                _liveDurationMs = req.durationMs;
                _inContinuousMode = true;
                _writeToneContinuous(req.durationMs);
                _inContinuousMode = false;
            }

            _playing = false;
        }
    }
}

void Speaker::_writeToneContinuous(int /*durationMs*/) {
    // Plays _liveFreqs, seamlessly picking up changes from playTone/playChord.
    // Uses a per-voice phase accumulator so frequency changes do NOT cause
    // a phase discontinuity (which would otherwise produce a sharp click each
    // time playChord/playTone updates the live freqs). Auto-stops when
    // _liveDurationMs ms have elapsed; live updates reset that budget.
    int16_t samples[128];  // 64 stereo pairs
    const float twoPi = 6.28318530717958647692f;

    // Phase accumulators per voice — start at 0 (silent → 0 sample is smooth)
    float phase[MAX_CHORD_NOTES] = {0.0f};

    long samplesRemaining = -1;  // -1 means "play forever"
    {
        int d = _liveDurationMs;
        if (d > 0) samplesRemaining = (long)_sampleRate * d / 1000;
    }

    while (!_stopRequested) {
        // Snapshot current live state
        int numFreqs = _liveNumFreqs;
        float freqs[MAX_CHORD_NOTES];
        for (int i = 0; i < numFreqs; i++) {
            freqs[i] = _liveFreqs[i];
        }
        bool wasUpdated = _liveUpdated;
        _liveUpdated = false;

        if (wasUpdated) {
            // New call came in — reset elapsed budget from latest duration.
            // Phase accumulators are intentionally NOT reset, so frequency
            // changes are continuous (no click).
            int d = _liveDurationMs;
            samplesRemaining = (d > 0) ? ((long)_sampleRate * d / 1000) : -1;
        }

        if (numFreqs == 0) {
            // All notes released — stop
            break;
        }
        if (samplesRemaining == 0) {
            // Duration elapsed — stop
            break;
        }

        float vol = _volume / numFreqs;

        // Pre-compute per-voice phase increments for this chunk batch
        float dPhase[MAX_CHORD_NOTES];
        for (int n = 0; n < numFreqs; n++) {
            dPhase[n] = (freqs[n] > 0) ? (twoPi * freqs[n] / _sampleRate) : 0.0f;
        }

        // Generate audio in chunks, checking for live updates between chunks
        for (int c = 0; c < 4 && !_stopRequested && !_liveUpdated; c++) {
            int chunk = 64;
            if (samplesRemaining > 0 && samplesRemaining < chunk) {
                chunk = (int)samplesRemaining;
            }
            for (int i = 0; i < chunk; i++) {
                float mixed = 0.0f;
                for (int n = 0; n < numFreqs; n++) {
                    if (dPhase[n] > 0.0f) {
                        mixed += sinf(phase[n]);
                        phase[n] += dPhase[n];
                        // Wrap phase to [0, 2π) to keep sinf() precise indefinitely
                        if (phase[n] >= twoPi) phase[n] -= twoPi;
                    }
                }
                int16_t sample = (int16_t)(mixed * 32767.0f * vol);
                samples[i * 2] = sample;
                samples[i * 2 + 1] = sample;
            }
            // Zero any unused stereo pairs in the buffer (if chunk < 64)
            for (int i = chunk; i < 64; i++) {
                samples[i * 2] = 0;
                samples[i * 2 + 1] = 0;
            }
            size_t bytesWritten = 0;
            i2s_write((i2s_port_t)_i2sPort, samples, chunk * 4, &bytesWritten, pdMS_TO_TICKS(50));

            if (samplesRemaining > 0) {
                samplesRemaining -= chunk;
                if (samplesRemaining <= 0) break;
            }
        }
        // Loop back to re-read _liveFreqs (picks up any changes seamlessly)
    }

    // Flush I2S buffer on stop / completion so the amp settles to silence
    int16_t silence[128] = {0};
    size_t bw;
    i2s_write((i2s_port_t)_i2sPort, silence, sizeof(silence), &bw, pdMS_TO_TICKS(20));

    // Clear live state so the next playTone/playChord call queues fresh
    _liveNumFreqs = 0;
    _liveDurationMs = 0;
}

void Speaker::_writeTone(const float* freqs, int numFreqs, int durationMs) {
    // Timed playback — used by playNote/RTTTL (not live-updated).
    // Applies a short linear fade-out at the tail so chained playNote() calls
    // don't click between notes (without the ramp, the tone ends at an
    // arbitrary sin() value, then the I2S DMA jumps to zero — that step is
    // exactly the click between notes in a melody).
    if (durationMs <= 0 || numFreqs <= 0) return;

    int totalSamples = (_sampleRate * durationMs) / 1000;
    // Fade-out the last 3 ms (or less if the note is shorter than 6 ms).
    int fadeSamples = min(totalSamples / 2, (_sampleRate * 3) / 1000);
    int fadeStart = totalSamples - fadeSamples;

    int16_t samples[128];  // 64 stereo pairs
    int samplesWritten = 0;

    float vol = _volume / numFreqs;

    while (samplesWritten < totalSamples && !_stopRequested) {
        int chunk = min(64, totalSamples - samplesWritten);
        for (int i = 0; i < chunk; i++) {
            float mixed = 0.0f;
            int sampleIdx = samplesWritten + i;
            for (int n = 0; n < numFreqs; n++) {
                if (freqs[n] > 0) {
                    mixed += sinf(2.0f * (float)M_PI * freqs[n] * sampleIdx / _sampleRate);
                }
            }
            float envelope = 1.0f;
            if (sampleIdx >= fadeStart && fadeSamples > 0) {
                envelope = (float)(totalSamples - sampleIdx) / (float)fadeSamples;
                if (envelope < 0.0f) envelope = 0.0f;
            }
            int16_t sample = (int16_t)(mixed * 32767.0f * vol * envelope);
            samples[i * 2] = sample;
            samples[i * 2 + 1] = sample;
        }
        size_t bytesWritten = 0;
        i2s_write((i2s_port_t)_i2sPort, samples, chunk * 4, &bytesWritten, pdMS_TO_TICKS(50));
        samplesWritten += chunk;
    }

    if (_stopRequested) {
        // Brief silence to settle the amp after an early stop.
        int16_t silence[128] = {0};
        size_t bw;
        i2s_write((i2s_port_t)_i2sPort, silence, sizeof(silence), &bw, pdMS_TO_TICKS(20));
    }
}

void Speaker::writeSamples(const int16_t* samples, int count) {
    if (!_initialized || count <= 0) return;

    int16_t stereo[512];  // 256 stereo pairs
    int written = 0;

    while (written < count) {
        int chunk = min(256, count - written);
        float vol = _volume;
        for (int i = 0; i < chunk; i++) {
            int16_t scaled = (int16_t)(samples[written + i] * vol);
            stereo[i * 2] = scaled;
            stereo[i * 2 + 1] = scaled;
        }
        size_t bytesWritten = 0;
        i2s_write((i2s_port_t)_i2sPort, stereo, chunk * 4, &bytesWritten, pdMS_TO_TICKS(100));
        written += chunk;
    }
}

void Speaker::playPCMBase64(const char* b64) {
    if (!_initialized) { Serial.println("[speaker] play_pcm: not initialized"); return; }
    if (!b64) { Serial.println("[speaker] play_pcm: null value"); return; }

    size_t inLen = strlen(b64);
    if (inLen == 0) { Serial.println("[speaker] play_pcm: empty value"); return; }

    size_t maxOut = (inLen / 4 + 1) * 3;
    uint8_t* buf = (uint8_t*)malloc(maxOut);
    if (!buf) { Serial.printf("[speaker] play_pcm: malloc failed (%u B)\n", (unsigned)maxOut); return; }

    size_t outLen = 0;
    int rc = mbedtls_base64_decode(buf, maxOut, &outLen,
                                   (const unsigned char*)b64, inLen);
    if (rc != 0) {
        Serial.printf("[speaker] play_pcm: base64 decode err=-0x%04x in=%u\n", -rc, (unsigned)inLen);
    } else if (outLen < 2) {
        Serial.printf("[speaker] play_pcm: decoded only %u B\n", (unsigned)outLen);
    } else {
        writeSamples((const int16_t*)buf, (int)(outLen / 2));
    }
    free(buf);
}

void Speaker::_writeSilence(int durationMs) {
    // Just delay — I2S auto-clear sends zeros when idle
    int elapsed = 0;
    while (elapsed < durationMs && !_stopRequested) {
        int chunk = min(10, durationMs - elapsed);
        vTaskDelay(pdMS_TO_TICKS(chunk));
        elapsed += chunk;
    }
}

// ========== RTTTL Playback ==========

int Speaker::_rtttlNoteFreq(char note, bool sharp, int octave) {
    // Base frequencies for octave 4
    // C4=262, D4=294, E4=330, F4=349, G4=392, A4=440, B4=494
    static const int baseFreqs[] = {
        262, 294, 330, 349, 392, 440, 494  // C, D, E, F, G, A, B
    };
    static const int sharpFreqs[] = {
        277, 311, 330, 370, 415, 466, 494  // C#, D#, E(no sharp), F#, G#, A#, B(no sharp)
    };

    int idx = -1;
    switch (note) {
        case 'c': idx = 0; break;
        case 'd': idx = 1; break;
        case 'e': idx = 2; break;
        case 'f': idx = 3; break;
        case 'g': idx = 4; break;
        case 'a': idx = 5; break;
        case 'b': idx = 6; break;
        default: return 0;  // pause/rest
    }

    int freq = sharp ? sharpFreqs[idx] : baseFreqs[idx];

    // Shift octave relative to octave 4
    int shift = octave - 4;
    if (shift > 0) {
        for (int i = 0; i < shift; i++) freq *= 2;
    } else if (shift < 0) {
        for (int i = 0; i < -shift; i++) freq /= 2;
    }

    return freq;
}

void Speaker::_rtttlTaskFunc(void* param) {
    Speaker* self = (Speaker*)param;
    self->_playRTTTLNotes(self->_rtttlMelody);
    self->_rtttlPlaying = false;
    free(self->_rtttlMelody);
    self->_rtttlMelody = nullptr;
    self->_rtttlTask = nullptr;
    vTaskDelete(NULL);
}

void Speaker::playRTTTL(const char* rtttl) {
    if (!_initialized || !rtttl) return;

    // Stop any current RTTTL playback
    if (_rtttlPlaying) {
        _stopRequested = true;
        // Wait for old task to finish
        int timeout = 200;
        while (_rtttlPlaying && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout -= 10;
        }
    }

    // Stop any single note that might be playing
    stop();

    // Copy the RTTTL string (task needs it to outlive this call)
    if (_rtttlMelody) {
        free(_rtttlMelody);
    }
    _rtttlMelody = strdup(rtttl);
    if (!_rtttlMelody) return;

    _rtttlPlaying = true;
    _stopRequested = false;

    // Launch RTTTL playback in a background task
    xTaskCreatePinnedToCore(
        _rtttlTaskFunc,
        "RTTTLTask",
        4096,
        this,
        1,       // Priority 1 (below audio task)
        &_rtttlTask,
        0        // Core 0
    );
}

void Speaker::_playRTTTLNotes(const char* rtttl) {
    // RTTTL format: name:d=N,o=N,b=N:note,note,...
    const char* p = rtttl;

    // Skip name section (everything before first ':')
    while (*p && *p != ':') p++;
    if (!*p) return;
    p++;  // skip ':'

    // Parse defaults section: d=N,o=N,b=N
    int defaultDuration = 4;
    int defaultOctave = 6;
    int bpm = 63;

    while (*p && *p != ':') {
        while (*p == ' ' || *p == ',') p++;
        if (*p == 'd' && *(p + 1) == '=') {
            p += 2;
            defaultDuration = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else if (*p == 'o' && *(p + 1) == '=') {
            p += 2;
            defaultOctave = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else if (*p == 'b' && *(p + 1) == '=') {
            p += 2;
            bpm = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else {
            p++;  // skip unknown
        }
    }
    if (!*p) return;
    p++;  // skip ':'

    // Whole note duration in ms = (60000 / bpm) * 4
    float wholeNote = (60000.0f / bpm) * 4.0f;

    // Parse and play notes
    while (*p && !_stopRequested) {
        // Skip whitespace and commas
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        // Parse optional duration number (before note letter)
        int duration = 0;
        while (*p >= '0' && *p <= '9') {
            duration = duration * 10 + (*p - '0');
            p++;
        }
        if (duration == 0) duration = defaultDuration;

        // Parse note letter
        char note = 0;
        if (*p) {
            note = (*p >= 'A' && *p <= 'G') ? (*p - 'A' + 'a') : *p;  // lowercase
            p++;
        }
        if (!note) break;

        // Parse optional sharp
        bool sharp = false;
        if (*p == '#') {
            sharp = true;
            p++;
        }

        // Parse optional dot (extends duration by 50%)
        bool dotted = false;
        if (*p == '.') {
            dotted = true;
            p++;
        }

        // Parse optional octave
        int octave = defaultOctave;
        if (*p >= '0' && *p <= '9') {
            octave = *p - '0';
            p++;
        }

        // Check for dot after octave too (some RTTTL variants)
        if (*p == '.') {
            dotted = true;
            p++;
        }

        // Calculate duration in ms
        int durationMs = (int)(wholeNote / duration);
        if (dotted) {
            durationMs = durationMs + durationMs / 2;
        }

        // Get frequency
        int freq = 0;
        if (note == 'p' || note == 'P') {
            freq = 0;  // pause
        } else {
            freq = _rtttlNoteFreq(note, sharp, octave);
        }

        // Play the note using the existing queue mechanism. We wait for the full
        // note duration after sending, so the queue normally only ever holds one
        // note at a time — xQueueSend with a small timeout is sufficient.
        if (freq > 0) {
            NoteRequest req = {};
            req.freqs[0] = (float)freq;
            req.numFreqs = 1;
            req.durationMs = durationMs;
            req.withGap = true;
            xQueueSend(_noteQueue, &req, pdMS_TO_TICKS(50));
            // Wait for note to finish
            vTaskDelay(pdMS_TO_TICKS(durationMs));
        } else {
            // Rest — just wait
            vTaskDelay(pdMS_TO_TICKS(durationMs));
        }
    }
}
