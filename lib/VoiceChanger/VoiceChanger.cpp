#include "VoiceChanger.h"

#include <math.h>

#include <algorithm>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kTargetPeak = 29204.0f; // -1 dBFS
constexpr float kMaxGain = 10.0f;       // +20 dB
constexpr size_t kFade = 128;           // 8 ms at each end
// One-pole high-pass: y = x - x' + p * y'. 0.97 puts the corner near
// 80 Hz at 16 kHz.
constexpr float kHighPassPole = 0.97f;
// Two biquads with these Qs make a 4th-order Butterworth low-pass.
constexpr float kButterQ1 = 0.5412f;
constexpr float kButterQ2 = 1.3066f;

inline int16_t clamp16(float v) {
    if (v >= 32767.0f) {
        return 32767;
    }
    if (v <= -32768.0f) {
        return -32768;
    }
    return static_cast<int16_t>(lrintf(v));
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

    readPos_ = 0;
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
// the samples at `ref` best (plain cross-correlation). A coarse search on
// every 4th sample and lag, then a fine one around the winner, costs about
// a tenth of the full search.
int VoiceChanger::bestOffset(size_t ref, long nominal) const {
    const long lo = std::max(0L, nominal - kSeek);
    const long hi = std::min(static_cast<long>(inLen_ - kFrame), nominal + kSeek);
    if (ref + kHop > inLen_ || lo > hi) {
        return static_cast<int>(std::min(std::max(nominal, lo), std::max(lo, hi)));
    }
    const int16_t *r = in_ + ref;

    long coarse = lo;
    float bestScore = -INFINITY;
    for (long c = lo; c <= hi; c += 4) {
        const int16_t *x = in_ + c;
        float s = 0;
        for (size_t i = 0; i < kHop; i += 4) {
            s += static_cast<float>(r[i]) * static_cast<float>(x[i]);
        }
        if (s > bestScore) {
            bestScore = s;
            coarse = c;
        }
    }

    long best = coarse;
    bestScore = -INFINITY;
    for (long c = std::max(lo, coarse - 3); c <= std::min(hi, coarse + 3); ++c) {
        const int16_t *x = in_ + c;
        float s = 0;
        for (size_t i = 0; i < kHop; ++i) {
            s += static_cast<float>(r[i]) * static_cast<float>(x[i]);
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

void VoiceChanger::resampleChunk() {
    const float wobble = static_cast<float>(2 * kPi * voice_.vibratoHz / kSampleRate);
    for (size_t n = 0; n < kHop; ++n) {
        const size_t i0 = static_cast<size_t>(readPos_);
        if (i0 + 1 >= stretchLen_ || outLen_ >= outCap_) {
            prepareFinish();
            return;
        }
        const float frac = static_cast<float>(readPos_ - static_cast<double>(i0));
        const float y = scratch_[i0] * (1.0f - frac) + scratch_[i0 + 1] * frac;
        const int16_t v = clamp16(y);
        out_[outLen_++] = v;
        peak_ = std::max(peak_, static_cast<int32_t>(v < 0 ? -static_cast<int32_t>(v) : v));

        double step = voice_.pitch;
        if (voice_.vibratoDepth > 0) {
            step *= 1.0 + voice_.vibratoDepth * sinf(wobble * static_cast<float>(outLen_));
        }
        readPos_ += step;
    }
}

void VoiceChanger::prepareFinish() {
    // Soft clipping maps the peak onto itself, so the gain can be chosen
    // before it is applied.
    gain_ = peak_ > 0 ? std::min(kTargetPeak / static_cast<float>(peak_), kMaxGain) : 0.0f;
    drivePeak_ = static_cast<float>(std::max<int32_t>(peak_, 1));
    driveNorm_ = voice_.drive > 0 ? 1.0f / tanhf(voice_.drive) : 1.0f;
    finishPos_ = 0;
    stage_ = outLen_ > 0 ? Stage::Finish : Stage::Done;
}

void VoiceChanger::finishChunk() {
    const size_t end = std::min(finishPos_ + kHop, outLen_);
    const size_t fade = std::min(kFade, outLen_ / 2);
    for (size_t i = finishPos_; i < end; ++i) {
        float x = out_[i];
        if (voice_.drive > 0) {
            x = drivePeak_ * tanhf(voice_.drive * x / drivePeak_) * driveNorm_;
        }
        x *= gain_;
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
