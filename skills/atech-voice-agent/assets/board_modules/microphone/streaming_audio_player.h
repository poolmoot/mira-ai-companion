/**
 * @file streaming_audio_player.h
 * @brief Jitter-buffered PCM stream player for Athera
 *
 * Plays 16-bit mono PCM that arrives in bursts — from a network, a host
 * over serial, or any other producer — through a Speaker without stutter.
 * Infrastructure class shared by other modules (e.g. VoiceAssistant); not
 * a placeable board module.
 *
 * Design:
 *   - enqueue() copies samples into a ~1 s ring and never blocks the caller
 *   - a dedicated playback task prebuffers ~300 ms, then streams to the
 *     Speaker; blocking on I2S DMA there paces output
 *   - if the buffer runs dry mid-stream, playback pauses and rebuffers —
 *     one short delay instead of continuous frame-by-frame stutter
 *   - endStream() marks the current stream complete, so a final tail
 *     shorter than the prebuffer still plays out; the next enqueue()
 *     implicitly starts a new stream
 *   - flush() discards everything buffered (teardown, interruptions)
 *
 * Threading: the ring is single-producer/single-consumer. All enqueue()/
 * endStream()/flush() calls must come from one task (the consumer is the
 * internal playback task). bufferedSamples() may be read from anywhere.
 * Buffer sizes assume the Speaker runs at 16 kHz (the platform default).
 */

#ifndef STREAMING_AUDIO_PLAYER_H
#define STREAMING_AUDIO_PLAYER_H

#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class Speaker;

class StreamingAudioPlayer {
public:
    static constexpr int RING_SAMPLES = 16384;      // ~1 s @ 16 kHz (RAM is tight: TLS needs ~50 KB)
    static constexpr int PREBUFFER_SAMPLES = 6400;  // 400 ms buffered before playback starts (rides out hotspot hiccups)
    static constexpr int CHUNK_SAMPLES = 960;       // 60 ms per speaker write
    static constexpr int STALL_TIMEOUT_MS = 250;    // sub-prebuffer audio plays after this stall

    explicit StreamingAudioPlayer(Speaker* speaker);

    /**
     * @brief Start the background playback task. Call once, after speaker.begin().
     */
    void begin();

    /**
     * @brief Queue PCM16LE mono bytes for playback. Never blocks.
     * If the ring is full the excess is dropped (and logged). Also marks the
     * stream as ongoing again after a previous endStream().
     */
    void enqueue(const uint8_t* data, size_t length);

    /**
     * @brief Declare the current stream complete (no more data coming).
     * Lets a buffered tail shorter than the prebuffer threshold play out
     * instead of waiting for more audio that will never arrive. Optional:
     * a tail also plays once the buffer stops growing for STALL_TIMEOUT_MS —
     * but calling this avoids that extra delay.
     */
    void endStream();

    /**
     * @brief Discard all buffered audio and stop playback until new data arrives.
     * Asynchronous: the playback task honors the request on its next
     * iteration, so up to one chunk (~60 ms) may still play after this
     * returns and bufferedSamples() drops to 0 shortly after, not instantly.
     */
    void flush();

    /**
     * @brief Samples currently waiting in the ring (0 = ring empty).
     * Note: up to ~128 ms more may still be playing out of the speaker's
     * I2S DMA buffer after this reaches 0.
     */
    int bufferedSamples();

    // Playback task loop (public for static trampoline, do not call directly)
    void _taskLoop();

private:
    Speaker* _speaker;

    // Single-producer (enqueue) / single-consumer (playback task) ring.
    // Atomics with release/acquire ordering: the sample stores must be
    // visible before the index publish crosses cores.
    int16_t* _ring;   // RING_SAMPLES, in PSRAM so internal RAM stays free for TLS
    std::atomic<int> _head;   // written by producer (enqueue)
    std::atomic<int> _tail;   // written by playback task
    int16_t _chunk[CHUNK_SAMPLES];

    TaskHandle_t _task;
    std::atomic<bool> _flushRequest;  // playback task discards buffered audio
    std::atomic<bool> _endOfStream;   // current stream is complete
    uint32_t _overflows;

    int _available();
    int _pop(int16_t* dest, int maxSamples);
};

#endif // STREAMING_AUDIO_PLAYER_H
