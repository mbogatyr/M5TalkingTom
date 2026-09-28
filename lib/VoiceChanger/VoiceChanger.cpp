// The Arduino core builds with -Os; the sample loops here run about twice
// as fast with -O2, and the voice change is on the critical path between
// the user going quiet and the character answering.
#pragma GCC optimize("O2")

#include "VoiceChanger.h"

#include <math.h>

#include <stdint.h>

#include <algorithm>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kTargetPeak = 29204.0f; // -1 dBFS
constexpr float kMaxGain = 10.0f;       // +20 dB in all
// Louder than peak normalisation alone: +8 dB, with a limiter holding the
// peaks at -1 dBFS. The tiny speaker needs every decibel, and quiet
// syllables come up closer to the loud ones.
constexpr float kLoudness = 2.5f;
// The limiter's envelope drops by 1/e in 40 ms after a peak.
constexpr float kLimiterRelease = 0.99844f;
constexpr size_t kFade = 128;           // 8 ms at each end
// One-pole high-pass: y = x - x' + p * y'. 0.97 puts the corner near
// 80 Hz at 16 kHz.
constexpr float kHighPassPole = 0.97f;
// Two biquads with these Qs make a 4th-order Butterworth low-pass.
constexpr float kButterQ1 = 0.5412f;
constexpr float kButterQ2 = 1.3066f;

// Rounds by hand: lrintf is a library call, and this runs for every sample.
inline int16_t clamp16(float v) {
    if (v >= 32767.0f) {
        return 32767;
    }
    if (v <= -32768.0f) {
        return -32768;
    }
    return static_cast<int16_t>(v >= 0 ? v + 0.5f : v - 0.5f);
}

// tanh by a Pade approximant, exact at 0 and at +-3 where it reaches +-1;
// tanhf costs about 2 us a sample on the board.
inline float softClip(float x) {
    if (x >= 3.0f) {
        return 1.0f;
    }
    if (x <= -3.0f) {
        return -1.0f;
    }
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

} // namespace

namespace voices {

const Voice &forCharacter(uint8_t character) {
    static const Voice all[kCount] = {kCat, kHippo, kMouse};
    return all[character % kCount];
}

} // namespace voices

float VoiceChanger::Biquad::run(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

// The low-pass from the RBJ audio EQ cookbook.
void VoiceChanger::Biquad::lowPass(float cutoffHz, float q) {
    const double w0 = 2 * kPi * cutoffHz / kSampleRate;
    const double cw = cos(w0);
    const double alpha = sin(w0) / (2 * q);
    const double a0 = 1 + alpha;
    b0 = static_cast<float>((1 - cw) / 2 / a0);
    b1 = static_cast<float>((1 - cw) / a0);
    b2 = b0;
    a1 = static_cast<float>(-2 * cw / a0);
    a2 = static_cast<float>((1 - alpha) / a0);
    z1 = z2 = 0;
}

VoiceChanger::VoiceChanger() {
    // A periodic Hann window: copies shifted by half its length add up to
    // exactly 1, so overlap-add keeps the level.
    for (size_t i = 0; i < kFrame; ++i) {
        window_[i] = static_cast<float>(0.5 - 0.5 * cos(2 * kPi * i / kFrame));
    }
}

size_t VoiceChanger::stretchCapacity(size_t inputLen, const Voice &voice) {
    return static_cast<size_t>(ceil(inputLen * voice.pitch / voice.tempo)) + 2 * kFrame;
}

size_t VoiceChanger::outputCapacity(size_t inputLen, const Voice &voice) {
    return static_cast<size_t>(ceil(inputLen / voice.tempo * (1 + voice.vibratoDepth))) +
           2 * kFrame;
}

void VoiceChanger::start(const int16_t *input, size_t inputLen, int16_t *scratch,
                         size_t scratchCap, int16_t *output, size_t outputCap,
                         const Voice &voice) {
    voice_ = voice;
    in_ = input;
    inLen_ = inputLen;
    scratch_ = scratch;
    scratchCap_ = scratchCap;
    out_ = output;
    outCap_ = outputCap;

    frame_ = 0;
    prevPos_ = 0;
    stretchLen_ = 0;
    const double stretch = static_cast<double>(voice.pitch) / voice.tempo;
    analysisHop_ = kHop / stretch;
    stretchTarget_ = std::min(scratchCap_, static_cast<size_t>(inputLen * stretch));
    std::fill(tail_, tail_ + (kFrame - kHop), 0.0f);

    filterPos_ = 0;
    hpPrevIn_ = hpPrevOut_ = 0;
    lowPassOn_ = voice.pitch > 1.0f;
    if (lowPassOn_) {
        const float cutoff = 0.9f * (kSampleRate / 2) / voice.pitch;
        lp1_.lowPass(cutoff, kButterQ1);
        lp2_.lowPass(cutoff, kButterQ2);
    }

    readIndex_ = 0;
    readFrac_ = 0;
    step_ = voice.pitch;
    outLen_ = 0;
    peak_ = 0;
    finishPos_ = 0;
    gain_ = 0;

    // Too short to window: nothing to say.
    stage_ = inputLen < 2 * kFrame ? Stage::Done : Stage::Stretch;
}

bool VoiceChanger::step(size_t work) {
    while (work-- > 0) {
        switch (stage_) {
        case Stage::Stretch:
            stretchFrame();
            break;
        case Stage::Filter:
            filterChunk();
            break;
        case Stage::Resample:
            resampleChunk();
            break;
        case Stage::Finish:
            finishChunk();
            break;
        case Stage::Idle:
            return false;
        case Stage::Done:
            return true;
        }
    }
    return stage_ == Stage::Done;
}

void VoiceChanger::stretchFrame() {
    const long nominal = lround(frame_ * analysisHop_);
    if (nominal + static_cast<long>(kFrame) > static_cast<long>(inLen_) ||
        stretchLen_ + kHop > stretchTarget_) {
        flushStretch();
        return;
    }
    // The first window starts the phrase; every later one is chosen to
    // continue its predecessor, which would have gone on at prevPos_ + kHop.
    const size_t pos = frame_ == 0 ? static_cast<size_t>(nominal)
                                   : static_cast<size_t>(bestOffset(prevPos_ + kHop, nominal));
    const int16_t *x = in_ + pos;
    int16_t *y = scratch_ + stretchLen_;
    for (size_t i = 0; i < kHop; ++i) {
        y[i] = clamp16(tail_[i] + window_[i] * x[i]);
    }
    for (size_t i = 0; i < kFrame - kHop; ++i) {
        tail_[i] = window_[kHop + i] * x[kHop + i];
    }
    stretchLen_ += kHop;
    prevPos_ = pos;
    ++frame_;
}

void VoiceChanger::flushStretch() {
    if (stretchLen_ + kHop <= scratchCap_) {
        for (size_t i = 0; i < kHop; ++i) {
            scratch_[stretchLen_ + i] = clamp16(tail_[i]);
        }
        stretchLen_ += kHop;
    }
    stage_ = Stage::Filter;
}

// The window start within +-kSeek of `nominal` whose first half matches
// the samples at `ref` best (plain cross-correlation, in integers). A coarse
// search on every 4th lag using every 8th sample, then a full one around the
// winner, costs about a twentieth of the full search. Speech has little
// energy above 1 kHz, so the coarse pass still lands next to the right lag.
int VoiceChanger::bestOffset(size_t ref, long nominal) const {
    const long lo = std::max(0L, nominal - kSeek);
    const long hi = std::min(static_cast<long>(inLen_ - kFrame), nominal + kSeek);
    if (ref + kHop > inLen_ || lo > hi) {
        return static_cast<int>(std::min(std::max(nominal, lo), std::max(lo, hi)));
    }
    const int16_t *r = in_ + ref;

    // Products are scaled down by 256 so that 256 of them fit an int32.
    long coarse = lo;
    int32_t bestScore = INT32_MIN;
    for (long c = lo; c <= hi; c += 4) {
        const int16_t *x = in_ + c;
        int32_t s = 0;
        for (size_t i = 0; i < kHop; i += 8) {
            s += (static_cast<int32_t>(r[i]) * x[i]) >> 8;
        }
        if (s > bestScore) {
            bestScore = s;
            coarse = c;
        }
    }

    long best = coarse;
    bestScore = INT32_MIN;
    for (long c = std::max(lo, coarse - 3); c <= std::min(hi, coarse + 3); ++c) {
        const int16_t *x = in_ + c;
        int32_t s = 0;
        for (size_t i = 0; i < kHop; ++i) {
            s += (static_cast<int32_t>(r[i]) * x[i]) >> 8;
        }
        if (s > bestScore) {
            bestScore = s;
            best = c;
        }
    }
    return static_cast<int>(best);
}

void VoiceChanger::filterChunk() {
    const size_t end = std::min(filterPos_ + kHop, stretchLen_);
    for (size_t i = filterPos_; i < end; ++i) {
        const float x = scratch_[i];
        float y = x - hpPrevIn_ + kHighPassPole * hpPrevOut_;
        hpPrevIn_ = x;
        hpPrevOut_ = y;
        if (lowPassOn_) {
            y = lp2_.run(lp1_.run(y));
        }
        scratch_[i] = clamp16(y);
    }
    filterPos_ = end;
    if (filterPos_ >= stretchLen_) {
        stage_ = Stage::Resample;
    }
}

// The ESP32-S3 has a single-precision FPU only, so the read position is an
// integer index plus a float fraction, and the vibrato's sine is taken once
// per 32 samples (2 ms), far finer than its 5-7 Hz wobble.
void VoiceChanger::resampleChunk() {
    constexpr size_t kWobbleEvery = 32;
    const float wobble = static_cast<float>(2 * kPi * voice_.vibratoHz / kSampleRate);
    for (size_t n = 0; n < kHop; ++n) {
        if (readIndex_ + 1 >= stretchLen_ || outLen_ >= outCap_) {
            prepareFinish();
            return;
        }
        const float a = scratch_[readIndex_];
        const float y = a + (scratch_[readIndex_ + 1] - a) * readFrac_;
        const int16_t v = clamp16(y);
        out_[outLen_++] = v;
        peak_ = std::max(peak_, static_cast<int32_t>(v < 0 ? -static_cast<int32_t>(v) : v));

        if (outLen_ % kWobbleEvery == 1) {
            step_ = voice_.pitch;
            if (voice_.vibratoDepth > 0) {
                step_ *= 1.0f + voice_.vibratoDepth * sinf(wobble * static_cast<float>(outLen_));
            }
        }
        readFrac_ += step_;
        const size_t whole = static_cast<size_t>(readFrac_); // floor: it is positive
        readIndex_ += whole;
        readFrac_ -= static_cast<float>(whole);
    }
}

void VoiceChanger::prepareFinish() {
    // Soft clipping maps the peak onto itself, so the gain can be chosen
    // before it is applied.
    gain_ = peak_ > 0 ? std::min(kTargetPeak * kLoudness / static_cast<float>(peak_), kMaxGain) : 0.0f;
    envelope_ = 0;
    drivePeak_ = static_cast<float>(std::max<int32_t>(peak_, 1));
    driveNorm_ = voice_.drive > 0 ? 1.0f / softClip(voice_.drive) : 1.0f;
    finishPos_ = 0;
    stage_ = outLen_ > 0 ? Stage::Finish : Stage::Done;
}

void VoiceChanger::finishChunk() {
    const size_t end = std::min(finishPos_ + kHop, outLen_);
    const size_t fade = std::min(kFade, outLen_ / 2);
    for (size_t i = finishPos_; i < end; ++i) {
        float x = out_[i];
        if (voice_.drive > 0) {
            x = drivePeak_ * softClip(voice_.drive * x / drivePeak_) * driveNorm_;
        }
        x *= gain_;
        // Peak limiter: instant attack, so no sample gets past the target.
        const float a = fabsf(x);
        envelope_ = std::max(a, envelope_ * kLimiterRelease);
        if (envelope_ > kTargetPeak) {
            x *= kTargetPeak / envelope_;
        }
        if (i < fade) {
            x *= static_cast<float>(i) / fade;
        } else if (i + fade >= outLen_) {
            x *= static_cast<float>(outLen_ - 1 - i) / fade;
        }
        out_[i] = clamp16(x);
    }
    finishPos_ = end;
    if (finishPos_ >= outLen_) {
        stage_ = Stage::Done;
    }
}
