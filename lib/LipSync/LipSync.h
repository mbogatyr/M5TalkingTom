#pragma once

#include <stddef.h>
#include <stdint.h>

// How wide the mouth opens while a phrase plays.
//
// analyze() goes over the finished phrase once, in 20 ms frames: each frame
// gets an opening from its loudness. The mouth opens at once on a sound and
// closes over a few frames, which looks like speech rather than flapping.
// During playback openness() is asked by the time since playback started.
class LipSync {
  public:
    static constexpr uint32_t kFrameMs = 20;
    static constexpr size_t kMaxFrames = 1024; // 20 s

    // Loudness range mapped onto closed..wide open. The phrase is normalised
    // to a -1 dBFS peak, so speech sits around -20..-10 dBFS.
    static constexpr float kClosedDbfs = -40.0f;
    static constexpr float kOpenDbfs = -12.0f;
    // How much the opening can drop per frame (255 = wide open).
    static constexpr uint8_t kCloseStep = 70;

    void analyze(const int16_t *samples, size_t len, uint32_t sampleRate);

    // 0 = closed, 255 = wide open; 0 after the end.
    uint8_t openness(uint32_t msSinceStart) const;

    size_t frames() const { return count_; }

  private:
    uint8_t levels_[kMaxFrames] = {};
    size_t count_ = 0;
};
