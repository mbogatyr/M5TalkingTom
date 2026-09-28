#include "PhraseRecorder.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace {

// The noise estimate falls quickly and rises slowly, so it follows the
// quiet moments between sounds rather than the sounds.
constexpr float kNoiseFall = 0.3f;         // share of the gap per block
constexpr float kNoiseRiseDb = 0.05f;      // dB per block, ~1.6 dB/s
constexpr float kNoiseMinDbfs = -90.0f;
constexpr float kNoiseMaxDbfs = -25.0f;
constexpr float kSilenceDbfs = -120.0f;

} // namespace

PhraseRecorder::PhraseRecorder(int16_t *buffer, size_t capacity, const Config &config)
    : cfg_(config), buffer_(buffer), capacity_(capacity) {
    preRollBlocks_ = blocksFor(cfg_.preRollMs) + cfg_.startBlocks;
    preRoll_.assign(preRollBlocks_ * cfg_.blockLen, 0);
    reset();
}

size_t PhraseRecorder::capacityFor(const Config &config) {
    PhraseRecorder probe(nullptr, 0, config);
    return (probe.preRollBlocks_ + probe.blocksFor(config.maxMs)) * config.blockLen;
}

uint32_t PhraseRecorder::blocksFor(uint32_t ms) const {
    const uint64_t samples = static_cast<uint64_t>(ms) * cfg_.sampleRate / 1000;
    return static_cast<uint32_t>((samples + cfg_.blockLen - 1) / cfg_.blockLen);
}

void PhraseRecorder::reset() {
    state_ = State::Idle;
    length_ = 0;
    voicedStart_ = voicedEnd_ = 0;
    quietBlocks_ = 0;
    loudRun_ = 0;
    peak_ = 0;
    preRollCount_ = 0;
    preRollNext_ = 0;
    settleLeft_ = blocksFor(cfg_.settleMs);
}

float PhraseRecorder::blockDbfs(const int16_t *block, size_t len) {
    uint64_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        const int32_t s = block[i];
        sum += static_cast<uint64_t>(s * s);
    }
    if (sum == 0 || len == 0) {
        return kSilenceDbfs;
    }
    const double rms = sqrt(static_cast<double>(sum) / static_cast<double>(len));
    return static_cast<float>(20.0 * log10(rms / 32768.0));
}

PhraseRecorder::Event PhraseRecorder::push(const int16_t *block) {
    level_ = blockDbfs(block, cfg_.blockLen);

    switch (state_) {
    case State::Holding:
        return Event::None;

    case State::Idle: {
        // The ES8311 gives pure zeros for a while after it is powered up:
        // they say nothing about the room.
        if (level_ <= kSilenceDbfs) {
            return Event::None;
        }
        remember(block);
        if (settleLeft_ > 0) {
            --settleLeft_;
            trackNoise(level_);
            return Event::None;
        }
        const float startAt = std::max(noise_ + cfg_.startAboveNoiseDb, cfg_.startMinDbfs);
        if (level_ >= startAt) {
            ++loudRun_;
        } else {
            loudRun_ = 0;
            trackNoise(level_);
        }
        if (loudRun_ >= cfg_.startBlocks) {
            startPhrase();
            return Event::Started;
        }
        return Event::None;
    }

    case State::Phrase: {
        if (length_ + cfg_.blockLen > capacity_) {
            return finish(true);
        }
        append(block);
        const float keepAt = std::max(noise_ + cfg_.keepAboveNoiseDb, cfg_.keepMinDbfs);
        if (level_ >= keepAt) {
            voicedEnd_ = length_;
            quietBlocks_ = 0;
        } else {
            ++quietBlocks_;
        }
        if (quietBlocks_ >= blocksFor(cfg_.hangMs)) {
            return finish(false);
        }
        const size_t maxSamples =
            static_cast<size_t>(static_cast<uint64_t>(cfg_.maxMs) * cfg_.sampleRate / 1000);
        if (length_ - voicedStart_ >= maxSamples || length_ + cfg_.blockLen > capacity_) {
            return finish(true);
        }
        return Event::None;
    }
    }
    return Event::None;
}

void PhraseRecorder::remember(const int16_t *block) {
    memcpy(&preRoll_[preRollNext_ * cfg_.blockLen], block, cfg_.blockLen * sizeof(int16_t));
    preRollNext_ = (preRollNext_ + 1) % preRollBlocks_;
    preRollCount_ = std::min(preRollCount_ + 1, preRollBlocks_);
}

void PhraseRecorder::startPhrase() {
    length_ = 0;
    peak_ = 0;
    size_t slot = (preRollNext_ + preRollBlocks_ - preRollCount_) % preRollBlocks_;
    for (size_t i = 0; i < preRollCount_ && length_ + cfg_.blockLen <= capacity_; ++i) {
        append(&preRoll_[slot * cfg_.blockLen]);
        slot = (slot + 1) % preRollBlocks_;
    }
    // The loud run that triggered the start is the newest part of it.
    const size_t run = cfg_.startBlocks * cfg_.blockLen;
    voicedStart_ = length_ > run ? length_ - run : 0;
    voicedEnd_ = length_;
    quietBlocks_ = 0;
    loudRun_ = 0;
    preRollCount_ = 0;
    state_ = State::Phrase;
}

void PhraseRecorder::append(const int16_t *block) {
    memcpy(buffer_ + length_, block, cfg_.blockLen * sizeof(int16_t));
    for (size_t i = 0; i < cfg_.blockLen; ++i) {
        const int32_t a = block[i] < 0 ? -static_cast<int32_t>(block[i]) : block[i];
        peak_ = std::max(peak_, a);
    }
    length_ += cfg_.blockLen;
}

PhraseRecorder::Event PhraseRecorder::finish(bool forced) {
    if (!forced) {
        const size_t tail =
            static_cast<size_t>(static_cast<uint64_t>(cfg_.tailMs) * cfg_.sampleRate / 1000);
        length_ = std::min(length_, voicedEnd_ + tail);
    }
    const uint64_t voicedMs =
        static_cast<uint64_t>(voicedEnd_ - voicedStart_) * 1000 / cfg_.sampleRate;
    if (voicedMs < cfg_.minMs) {
        state_ = State::Idle;
        length_ = 0;
        loudRun_ = 0;
        preRollCount_ = 0;
        return Event::Discarded;
    }
    state_ = State::Holding;
    return Event::Ended;
}

void PhraseRecorder::trackNoise(float level) {
    if (!noiseKnown_) {
        noise_ = level;
        noiseKnown_ = true;
    } else if (level < noise_) {
        noise_ += (level - noise_) * kNoiseFall;
    } else {
        noise_ += std::min(level - noise_, kNoiseRiseDb);
    }
    noise_ = std::min(std::max(noise_, kNoiseMinDbfs), kNoiseMaxDbfs);
}

uint32_t PhraseRecorder::lengthMs() const {
    return static_cast<uint32_t>(static_cast<uint64_t>(length_) * 1000 / cfg_.sampleRate);
}

float PhraseRecorder::peakDbfs() const {
    if (peak_ <= 0) {
        return kSilenceDbfs;
    }
    return static_cast<float>(20.0 * log10(peak_ / 32768.0));
}
