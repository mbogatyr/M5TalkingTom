#include "LipSync.h"

#include <math.h>

#include <algorithm>

void LipSync::analyze(const int16_t *samples, size_t len, uint32_t sampleRate) {
    const size_t frameLen = sampleRate * kFrameMs / 1000;
    count_ = std::min(kMaxFrames, frameLen > 0 ? len / frameLen : 0);
    int previous = 0;
    for (size_t f = 0; f < count_; ++f) {
        const int16_t *s = samples + f * frameLen;
        double sum = 0;
        for (size_t i = 0; i < frameLen; ++i) {
            sum += static_cast<double>(s[i]) * s[i];
        }
        const double rms = sqrt(sum / frameLen);
        const float db = rms > 0 ? static_cast<float>(20.0 * log10(rms / 32768.0)) : -120.0f;
        float open = (db - kClosedDbfs) / (kOpenDbfs - kClosedDbfs);
        open = std::min(std::max(open, 0.0f), 1.0f);
        const int raw = static_cast<int>(lrintf(open * 255.0f));
        // Opens at once, closes gradually.
        const int level = std::max(raw, previous - kCloseStep);
        levels_[f] = static_cast<uint8_t>(level);
        previous = level;
    }
}

uint8_t LipSync::openness(uint32_t msSinceStart) const {
    const uint32_t f = msSinceStart / kFrameMs;
    if (f >= count_) {
        return 0;
    }
    // Straight line between this frame and the next, for smooth motion at
    // any frame rate.
    const uint32_t into = msSinceStart % kFrameMs;
    const int a = levels_[f];
    const int b = f + 1 < count_ ? levels_[f + 1] : 0;
    return static_cast<uint8_t>(a + (b - a) * static_cast<int>(into) / static_cast<int>(kFrameMs));
}
