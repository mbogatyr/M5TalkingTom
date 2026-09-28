#pragma once

#include <M5Unified.h>

#include "BlockRing.h"

// Capture from the StickS3's built-in mic (ES8311 codec) at 16 kHz.
//
// The mic records into blocks of 512 samples (32 ms) in an M5Unified
// background task; next() keeps its queue full and hands out the finished
// blocks in order. Requests have an even length on purpose: M5Unified's mono
// capture yields samples in pairs, and odd lengths once left a wrong sample
// every few seconds (found in M5VoiceRecorder).
//
// The speaker shares the mic's I2S clock lines, so the two take turns:
// stop() before playback, start() after it. After start() the ES8311 gives
// zeros for a while; PhraseRecorder ignores them.
class AudioCapture {
  public:
    static constexpr uint32_t kSampleRate = 16000;
    static constexpr size_t kBlockLen = 512;
    // Two blocks in the mic queue plus room for the loop to fall behind:
    // a frame push takes about 15 ms, a block lasts 32 ms.
    static constexpr size_t kRingBlocks = BlockRing::kInFlight + 6;

    // Call once after M5.begin(). Returns false if the board has no mic.
    bool begin();

    bool start();
    void stop();
    bool running() const { return running_; }

    // Tops up the mic queue and returns the next finished block, or nullptr
    // when there is none yet. Call it until it returns nullptr.
    const int16_t *next();

    // Blocks whose slot was reused before they were read: audio really lost.
    uint32_t lostBlocks() const { return lost_; }

  private:
    BlockRing ring_{kBlockLen, kRingBlocks};
    uint32_t nextSeq_ = 0;
    uint32_t lost_ = 0;
    bool running_ = false;
};
