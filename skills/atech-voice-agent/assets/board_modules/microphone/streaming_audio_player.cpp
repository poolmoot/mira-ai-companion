/**
 * @file streaming_audio_player.cpp
 * @brief Jitter-buffered PCM stream player implementation
 */

#include <esp_heap_caps.h>
#include "streaming_audio_player.h"
#include "speaker.h"
#include <Arduino.h>
#include <string.h>

static void playerTaskTrampoline(void* param) {
    ((StreamingAudioPlayer*)param)->_taskLoop();
}

StreamingAudioPlayer::StreamingAudioPlayer(Speaker* speaker)
    : _speaker(speaker)
    , _head(0)
    , _tail(0)
    , _task(nullptr)
    , _flushRequest(false)
    , _endOfStream(false)
    , _overflows(0)
{
    _ring = (int16_t*)heap_caps_calloc(RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!_ring) _ring = (int16_t*)calloc(RING_SAMPLES, sizeof(int16_t));
}

void StreamingAudioPlayer::begin() {
    if (!_speaker) {
        Serial.println("[AudioStream] No speaker — player disabled");
        return;
    }
    if (_task) return;
    // Priority 3: above the speaker's tone task (2) — stream timing matters
    // more than beeps. Spends most of its life blocked on I2S DMA.
    xTaskCreatePinnedToCore(playerTaskTrampoline, "AudioStream", 4096, this, 3, &_task, 0);
}

void StreamingAudioPlayer::enqueue(const uint8_t* data, size_t length) {
    if (!_speaker || length < 2) return;

    // New data means the stream is (still) running — re-arm the prebuffer
    // semantics for a stream that previously ended.
    _endOfStream.store(false, std::memory_order_relaxed);

    size_t samples = length / 2;
    int head = _head.load(std::memory_order_relaxed);
    int tail = _tail.load(std::memory_order_acquire);
    size_t written = 0;
    while (written < samples) {
        int next = (head + 1) % RING_SAMPLES;
        if (next == tail) break;  // ring full — drop the rest
        // Byte-wise copy: the source pointer has no alignment guarantee
        int16_t s;
        memcpy(&s, data + written * 2, 2);
        _ring[head] = s;
        head = next;
        written++;
    }
    // Release: sample stores above must be visible before the index publish
    _head.store(head, std::memory_order_release);
    if (written < samples && _overflows++ % 50 == 0) {
        Serial.println("[AudioStream] Buffer overflow — dropping audio");
    }
}

void StreamingAudioPlayer::endStream() {
    _endOfStream.store(true, std::memory_order_release);
}

void StreamingAudioPlayer::flush() {
    _flushRequest.store(true, std::memory_order_release);
}

int StreamingAudioPlayer::bufferedSamples() {
    return _available();
}

void StreamingAudioPlayer::_taskLoop() {
    bool primed = false;
    int lastAvailable = -1;
    uint32_t lastGrowthMs = millis();
    while (true) {
        if (_flushRequest.load(std::memory_order_acquire)) {
            _flushRequest.store(false, std::memory_order_relaxed);
            // Re-read head until stable so samples being enqueued concurrently
            // with the flush don't survive as stale audio in the next stream.
            int head = _head.load(std::memory_order_acquire);
            do {
                _tail.store(head, std::memory_order_release);
                head = _head.load(std::memory_order_acquire);
            } while (head != _tail.load(std::memory_order_relaxed));
            primed = false;
            lastAvailable = -1;
            lastGrowthMs = millis();
        }
        int available = _available();
        if (!primed) {
            // Prebuffer before (re)starting playback: turns network jitter
            // into a one-time delay instead of continuous stutter. Whatever
            // is buffered plays out even below the threshold when the
            // producer declared the stream complete — or when the buffer
            // simply stops growing (watchdog, so audio can never be
            // stranded by a missed endStream()).
            uint32_t now = millis();
            if (available != lastAvailable) {
                lastAvailable = available;
                lastGrowthMs = now;
            }
            bool streamComplete = available > 0 &&
                (_endOfStream.load(std::memory_order_acquire) ||
                 (now - lastGrowthMs) >= (uint32_t)STALL_TIMEOUT_MS);
            if (available < PREBUFFER_SAMPLES && !streamComplete) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            primed = true;
        }
        if (available == 0) {
            primed = false;  // ran dry (stream ended, or underrun) — rebuffer
            if (!_endOfStream.load(std::memory_order_acquire)) {
                Serial.println("[AudioStream] Underrun — rebuffering");
            }
            continue;
        }
        int n = _pop(_chunk, CHUNK_SAMPLES);
        _speaker->writeSamples(_chunk, n);  // blocks on I2S DMA — paces playback
    }
}

int StreamingAudioPlayer::_available() {
    int head = _head.load(std::memory_order_acquire);
    int tail = _tail.load(std::memory_order_relaxed);
    return head >= tail ? head - tail : head + RING_SAMPLES - tail;
}

int StreamingAudioPlayer::_pop(int16_t* dest, int maxSamples) {
    int available = _available();
    int count = available < maxSamples ? available : maxSamples;
    int tail = _tail.load(std::memory_order_relaxed);
    for (int i = 0; i < count; i++) {
        dest[i] = _ring[tail];
        tail = (tail + 1) % RING_SAMPLES;
    }
    // Release: sample reads above must complete before the slots are reusable
    _tail.store(tail, std::memory_order_release);
    return count;
}
