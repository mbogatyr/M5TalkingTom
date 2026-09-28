#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

// Ring of blocks for continuous recording from the microphone.
//
// M5.Mic.record() accepts up to two requests ahead and fills them in turn
// without gaps. The firmware keeps the queue full, so of all queued blocks
// the two most recent are still being written and everything older is
// complete. The ring counts how many blocks have been queued and uses that
// count to hand out the complete blocks one by one, in order.
//
// Taken from M5VoiceRecorder (and M5SpectrumAnalyzer before it). Here the
// microphone is stopped for every playback, so the ring can start over.
class BlockRing {
  public:
    // Number of blocks in the microphone queue at any one time.
    static constexpr size_t kInFlight = 2;

    BlockRing(size_t blockLen, size_t blockCount);

    size_t blockLen() const { return blockLen_; }

    // Buffer for the next record() call.
    int16_t *nextWriteBlock();

    // record() has accepted the buffer from nextWriteBlock().
    void markQueued();

    // Forgets every block: the microphone was stopped and its queue
    // dropped. Counting starts from 0 again.
    void reset();

    // Total number of blocks fully written so far.
    uint32_t completedBlocks() const;

    // Complete block number `seq` (counting from 0), or nullptr if it is
    // not complete yet or its slot has already been handed to the
    // microphone again.
    const int16_t *block(uint32_t seq) const;

  private:
    size_t blockLen_;
    size_t blockCount_;
    std::vector<int16_t> samples_;
    uint32_t queued_ = 0;
};
