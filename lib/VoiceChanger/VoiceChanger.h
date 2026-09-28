#pragma once

#include <stddef.h>
#include <stdint.h>

// How a character changes the voice.
struct Voice {
    float pitch;        // frequency ratio: 2 is an octave up
    float tempo;        // speed ratio: the output lasts input / tempo
    float vibratoHz;    // pitch wobble rate, 0 for none
    float vibratoDepth; // wobble depth as a share of the pitch
    float drive;        // soft clipping for a gritty voice, 0 for none
};

// The three characters, in the order KEY1 walks through them.
namespace voices {
constexpr uint8_t kCount = 3;
constexpr Voice kCat{1.6f, 1.05f, 5.0f, 0.02f, 0.0f};
constexpr Voice kHippo{0.62f, 0.9f, 0.0f, 0.0f, 2.5f};
constexpr Voice kMouse{2.2f, 1.2f, 7.0f, 0.03f, 0.0f};
const Voice &forCharacter(uint8_t character);
} // namespace voices

// Turns a recorded phrase into a character's voice.
//
// The pitch moves while the tempo stays about the same:
//  1. WSOLA stretches the phrase in time by pitch / tempo: overlapping
//     windows are cut from the input and each one is shifted by up to 8 ms
//     so that it continues the previous one smoothly.
//  2. A high-pass takes away rumble the tiny speaker cannot play, and when
//     the pitch goes up, a low-pass keeps what would fold over the Nyquist
//     frequency out.
//  3. Resampling by the pitch squeezes the stretched phrase back to the
//     tempo's length and raises (or lowers) every frequency, formants
//     included: that is what makes a cartoon voice rather than a sped-up
//     tape. The vibrato wobbles the resampling step.
//  4. Optional soft clipping, then the whole phrase is scaled 8 dB above
//     the gain that would put its peak at -1 dBFS (but never by more than
//     +20 dB in all, so a quiet phrase does not turn into loud hiss); a
//     peak limiter keeps it at -1 dBFS. Short fades at both ends.
//
// The work is split into small steps so that the caller can keep drawing
// frames in between. All buffers belong to the caller.
class VoiceChanger {
  public:
    static constexpr uint32_t kSampleRate = 16000;
    static constexpr size_t kFrame = 512; // 32 ms window
    static constexpr size_t kHop = 256;   // 50 % overlap
    static constexpr int kSeek = 128;     // +-8 ms search

    VoiceChanger();

    static size_t stretchCapacity(size_t inputLen, const Voice &voice);
    static size_t outputCapacity(size_t inputLen, const Voice &voice);

    void start(const int16_t *input, size_t inputLen, int16_t *scratch, size_t scratchCap,
               int16_t *output, size_t outputCap, const Voice &voice);

    // Does up to `work` units (a WSOLA frame, or 256 samples of a later
    // pass). Returns true once the output is complete.
    bool step(size_t work);

    bool done() const { return stage_ == Stage::Done; }
    // 0 idle, 1 stretch, 2 filter, 3 resample, 4 finish, 5 done: for
    // timing the passes on the board.
    int stage() const { return static_cast<int>(stage_); }
    const int16_t *output() const { return out_; }
    size_t length() const { return outLen_; }

  private:
    enum class Stage : uint8_t { Idle, Stretch, Filter, Resample, Finish, Done };

    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;
        float run(float x);
        void lowPass(float cutoffHz, float q);
    };

    void stretchFrame();
    void flushStretch();
    int bestOffset(size_t ref, long nominal) const;
    void filterChunk();
    void resampleChunk();
    void finishChunk();
    void prepareFinish();

    float window_[kFrame];
    float tail_[kFrame - kHop];

    Voice voice_{1, 1, 0, 0, 0};
    const int16_t *in_ = nullptr;
    size_t inLen_ = 0;
    int16_t *scratch_ = nullptr;
    size_t scratchCap_ = 0;
    int16_t *out_ = nullptr;
    size_t outCap_ = 0;

    Stage stage_ = Stage::Idle;

    // Stretch.
    size_t frame_ = 0;
    size_t prevPos_ = 0;
    size_t stretchTarget_ = 0;
    size_t stretchLen_ = 0;
    double analysisHop_ = kHop;

    // Filter.
    size_t filterPos_ = 0;
    bool lowPassOn_ = false;
    Biquad lp1_, lp2_;
    float hpPrevIn_ = 0, hpPrevOut_ = 0;

    // Resample.
    size_t readIndex_ = 0;
    float readFrac_ = 0;
    float step_ = 1;
    size_t outLen_ = 0;
    int32_t peak_ = 0;

    // Finish.
    size_t finishPos_ = 0;
    float gain_ = 0;
    float drivePeak_ = 1;
    float driveNorm_ = 1;
    float envelope_ = 0;
};
