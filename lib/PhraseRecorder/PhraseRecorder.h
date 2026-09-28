#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

// Finds a spoken phrase in the microphone stream and keeps its samples.
//
// Blocks of samples go in one by one; the recorder says when a phrase has
// started and when it is over. A phrase starts when a block is well above
// the background noise, and ends after enough quiet to tell a finished
// sentence from a pause between words. The noise level is tracked all the
// time, so a humming fridge does not count as speech.
//
// A short stretch before the start (the pre-roll) is kept too: the first
// syllable is usually quieter than the threshold. The phrase buffer belongs
// to the caller (on the board it lives in PSRAM).
//
// Like everything in lib/, it knows nothing about the hardware: time is
// counted in blocks.
class PhraseRecorder {
  public:
    struct Config {
        uint32_t sampleRate = 16000;
        size_t blockLen = 512; // 32 ms at 16 kHz

        // A phrase starts once startBlocks blocks in a row reach
        // max(noise + startAboveNoiseDb, startMinDbfs), and goes on while
        // blocks reach max(noise + keepAboveNoiseDb, keepMinDbfs).
        float startAboveNoiseDb = 15.0f;
        float startMinDbfs = -45.0f;
        float keepAboveNoiseDb = 9.0f;
        float keepMinDbfs = -50.0f;
        uint32_t startBlocks = 2;

        uint32_t preRollMs = 320;
        uint32_t hangMs = 700;  // quiet that ends a phrase
        uint32_t tailMs = 150;  // quiet kept after the last loud block
        uint32_t minMs = 350;   // shorter phrases are dropped as clicks
        uint32_t maxMs = 8000;  // longer ones are cut here

        // After reset() the first blocks only feed the noise estimate.
        uint32_t settleMs = 250;
    };

    enum class Event : uint8_t {
        None,
        Started,   // a phrase has begun
        Ended,     // samples() holds a whole phrase; call reset() when done
        Discarded, // it was too short: back to listening
    };

    // buffer must hold at least capacityFor(config) samples.
    PhraseRecorder(int16_t *buffer, size_t capacity, const Config &config);
    explicit PhraseRecorder(int16_t *buffer, size_t capacity)
        : PhraseRecorder(buffer, capacity, Config{}) {}

    static size_t capacityFor(const Config &config);

    // Feeds one block of config.blockLen samples.
    Event push(const int16_t *block);

    // Starts listening again: after a phrase has been used, and after the
    // microphone was restarted. The noise estimate is kept.
    void reset();

    bool inPhrase() const { return state_ == State::Phrase; }
    bool holding() const { return state_ == State::Holding; }

    const int16_t *samples() const { return buffer_; }
    size_t length() const { return length_; }
    uint32_t lengthMs() const;
    float peakDbfs() const;

    // Level of the last block and the noise estimate, in dBFS.
    float level() const { return level_; }
    float noiseFloor() const { return noise_; }

    // RMS of a block in dBFS; silence gives -120.
    static float blockDbfs(const int16_t *block, size_t len);

  private:
    enum class State : uint8_t { Idle, Phrase, Holding };

    uint32_t blocksFor(uint32_t ms) const;
    void remember(const int16_t *block);
    void startPhrase();
    void append(const int16_t *block);
    Event finish(bool forced);
    void trackNoise(float level);

    Config cfg_;
    int16_t *buffer_;
    size_t capacity_;

    State state_ = State::Idle;
    size_t length_ = 0;
    size_t voicedEnd_ = 0;   // just past the last loud block
    size_t voicedStart_ = 0; // start of the first loud block
    uint32_t quietBlocks_ = 0;
    uint32_t loudRun_ = 0;
    uint32_t settleLeft_ = 0;
    int32_t peak_ = 0;

    float level_ = -120.0f;
    float noise_ = -60.0f;
    bool noiseKnown_ = false;

    // Pre-roll ring of whole blocks.
    std::vector<int16_t> preRoll_;
    size_t preRollBlocks_ = 0;
    size_t preRollCount_ = 0;
    size_t preRollNext_ = 0;
};
