/**
 * @file speaker.h
 * @brief I2S Speaker Module (MAX98357A) for Athera
 *
 * Non-blocking I2S audio amplifier for playing tones and melodies.
 * Audio plays in a background FreeRTOS task so the main loop stays responsive.
 * Double module spanning two adjacent ports.
 *
 * Specifications:
 * - Interface: I2S (3 pins)
 * - Amplifier: MAX98357A (3W Class D)
 * - Sample Rate: 16000 Hz (default, configurable)
 * - Bit Depth: 16-bit mono
 *
 * Athera Connector (double module):
 * - Port 1 Line A: LRCLK (left/right clock)
 * - Port 1 Line B: BCLK (bit clock)
 * - Port 2 Line B: DIN (data in)
 */

#ifndef SPEAKER_MODULE_H
#define SPEAKER_MODULE_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// Note frequency constants (Hz)
#define NOTE_C3  131
#define NOTE_D3  147
#define NOTE_E3  165
#define NOTE_F3  175
#define NOTE_G3  196
#define NOTE_A3  220
#define NOTE_B3  247

#define NOTE_C4  262
#define NOTE_CS4 277
#define NOTE_D4  294
#define NOTE_DS4 311
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_FS4 370
#define NOTE_G4  392
#define NOTE_GS4 415
#define NOTE_A4  440
#define NOTE_AS4 466
#define NOTE_B4  494

#define NOTE_C5  523
#define NOTE_CS5 554
#define NOTE_D5  587
#define NOTE_DS5 622
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_FS5 740
#define NOTE_G5  784
#define NOTE_GS5 831
#define NOTE_A5  880
#define NOTE_AS5 932
#define NOTE_B5  988

#define NOTE_C6  1047
#define NOTE_D6  1175
#define NOTE_E6  1319
#define NOTE_F6  1397
#define NOTE_G6  1568
#define NOTE_A6  1760
#define NOTE_B6  1976

#define NOTE_REST 0

// Max simultaneous notes in a chord
#define MAX_CHORD_NOTES 6

// Internal note request struct
struct NoteRequest {
    float freqs[MAX_CHORD_NOTES];  // Frequencies (0 = unused slot)
    int numFreqs;                   // Number of active frequencies
    int durationMs;
    bool withGap;  // true = playNote (85% sound, 15% gap), false = playTone (continuous)
};

class Speaker {
public:
    static constexpr int DEFAULT_SAMPLE_RATE = 16000;

    /**
     * @brief Construct Speaker module
     * @param bclkPin I2S bit clock pin (Line B of primary port)
     * @param lrcPin I2S left/right clock pin (Line A of primary port)
     * @param doutPin I2S data out pin (Line B of secondary port)
     */
    Speaker(int bclkPin, int lrcPin, int doutPin);

    /**
     * @brief Initialize I2S driver and start background audio task
     * @param sampleRate Sample rate in Hz (default 16000, use same rate as mic for streaming)
     */
    void begin(int sampleRate = DEFAULT_SAMPLE_RATE);

    /**
     * @brief Play a tone at given frequency (non-blocking)
     * @param freq Frequency in Hz (use NOTE_* constants or raw value)
     * @param durationMs Duration in milliseconds
     */
    void playTone(float freq, int durationMs);

    /**
     * @brief Play a musical note with automatic gap (non-blocking)
     * @param freq Frequency in Hz
     * @param durationMs Total note duration (plays 85%, silent 15%)
     */
    void playNote(float freq, int durationMs);

    /**
     * @brief Play a chord (multiple notes simultaneously, non-blocking)
     * @param freqs Array of frequencies in Hz
     * @param numFreqs Number of frequencies (max MAX_CHORD_NOTES)
     * @param durationMs Duration in milliseconds
     */
    void playChord(const float* freqs, int numFreqs, int durationMs);

    /**
     * @brief Stop any currently playing sound immediately
     */
    void stop();

    /**
     * @brief Check if a note is currently playing
     * @return true if audio is actively playing
     */
    bool isPlaying();

    /**
     * @brief Set volume level (persists across calls)
     * @param vol Volume from 0.0 (silent) to 1.0 (max)
     */
    void setVolume(float vol);

    /**
     * @brief Get current volume
     * @return Current volume (0.0 to 1.0)
     */
    float getVolume();

    /**
     * @brief Write raw audio samples directly to I2S (blocking)
     * @param samples Pointer to 16-bit mono samples
     * @param count Number of samples
     *
     * Use this to stream audio from a microphone. Samples are duplicated
     * to both L/R channels and volume-scaled before output.
     */
    void writeSamples(const int16_t* samples, int count);

    /**
     * @brief Decode a base64-encoded chunk of 16-bit mono PCM and play it (blocking).
     * @param b64 Null-terminated base64 string. Decoded bytes are interpreted as
     *            signed little-endian 16-bit PCM at the speaker's sample rate
     *            (16 kHz default — host must resample MP3/audio to match).
     *
     * For streaming MP3/audio from a host (browser via Web Serial, or app via WS):
     * host decodes the file to 16 kHz mono int16 PCM, then sends a sequence of
     * `{"action":"play_pcm","value":"<base64>"}` messages. The host paces sends to
     * roughly the audio duration per chunk so I2S DMA never starves. Recommended
     * chunk size: 256–512 samples (~16–32 ms at 16 kHz).
     */
    void playPCMBase64(const char* b64);

    /**
     * @brief Play an RTTTL melody string (non-blocking)
     * @param rtttl Full RTTTL string e.g. "name:d=4,o=5,b=140:c,e,g"
     *
     * Parses the RTTTL format and queues notes for background playback.
     * Call isPlaying() to check if melody is still playing.
     * Call stop() to interrupt.
     */
    void playRTTTL(const char* rtttl);

    // Background task function (public for static callback, do not call directly)
    void _audioTaskLoop();

private:
    int _i2sPort;  // I2S_NUM_0 or I2S_NUM_1 (auto-assigned)
    int _bclkPin;
    int _lrcPin;
    int _doutPin;
    int _sampleRate;

    static int _nextI2SPort;  // Auto-assigns I2S peripherals (max 2 speakers)
    volatile float _volume;
    bool _initialized;
    volatile bool _playing;
    volatile bool _stopRequested;

    // Live frequency state — updated by playChord/playTone without stopping audio
    volatile float _liveFreqs[MAX_CHORD_NOTES];
    volatile int _liveNumFreqs;
    volatile int _liveDurationMs;  // <=0 = play until stop(); >0 = auto-stop after this many ms
    volatile bool _liveUpdated;  // Flag: new frequencies available
    volatile bool _inContinuousMode;  // true while audio task is in _writeToneContinuous (live-updatable)

    QueueHandle_t _noteQueue;
    TaskHandle_t _audioTask;
    TaskHandle_t _rtttlTask;

    void _writeToneContinuous(int durationMs);  // Plays _liveFreqs until stopped/updated/duration elapses
    void _writeTone(const float* freqs, int numFreqs, int durationMs);  // Timed playback (for RTTTL/playNote)
    void _writeSilence(int durationMs);

    // RTTTL parser helpers
    static int _rtttlNoteFreq(char note, bool sharp, int octave);
    static void _rtttlTaskFunc(void* param);

    // RTTTL melody storage (copied for background playback)
    char* _rtttlMelody;
    volatile bool _rtttlPlaying;
    void _playRTTTLNotes(const char* rtttl);
};

#endif // SPEAKER_MODULE_H
