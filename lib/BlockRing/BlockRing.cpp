#include "BlockRing.h"

BlockRing::BlockRing(size_t blockLen, size_t blockCount)
    : blockLen_(blockLen), blockCount_(blockCount),
      samples_(blockLen * blockCount, 0) {}

int16_t *BlockRing::nextWriteBlock() {
    return samples_.data() + (queued_ % blockCount_) * blockLen_;
}

void BlockRing::markQueued() { ++queued_; }

void BlockRing::reset() { queued_ = 0; }

uint32_t BlockRing::completedBlocks() const {
    return (queued_ > kInFlight) ? queued_ - kInFlight : 0;
}

const int16_t *BlockRing::block(uint32_t seq) const {
    if (seq >= completedBlocks()) {
        return nullptr;
    }
    // Queued blocks reuse slots: once blockCount blocks have been queued
    // after seq, its slot belongs to the microphone again.
    if (queued_ - seq > blockCount_) {
        return nullptr;
    }
    return samples_.data() + (seq % blockCount_) * blockLen_;
}
