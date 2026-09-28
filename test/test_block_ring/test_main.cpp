#include <unity.h>

#include "BlockRing.h"

// Blocks of 3 samples in a ring of 5. The test plays the microphone: it
// writes into each block its sequence number times 10 plus the sample
// index, so every value shows where it came from.

static const size_t kLen = 3;
static const size_t kCount = 5;

static void queueBlocks(BlockRing &ring, int from, int to) {
    for (int seq = from; seq < to; ++seq) {
        int16_t *block = ring.nextWriteBlock();
        for (size_t i = 0; i < kLen; ++i) {
            block[i] = static_cast<int16_t>(seq * 10 + i);
        }
        ring.markQueued();
    }
}

void setUp(void) {}
void tearDown(void) {}

void test_write_blocks_walk_the_ring_and_wrap(void) {
    BlockRing ring(kLen, kCount);
    int16_t *first = ring.nextWriteBlock();

    ring.markQueued();
    TEST_ASSERT_EQUAL_PTR(first + kLen, ring.nextWriteBlock());

    for (size_t i = 1; i < kCount; ++i) {
        ring.markQueued();
    }
    TEST_ASSERT_EQUAL_PTR(first, ring.nextWriteBlock());
}

// The two most recently queued blocks are still being written.
void test_blocks_in_the_queue_are_not_complete(void) {
    BlockRing ring(kLen, kCount);

    queueBlocks(ring, 0, 2);
    TEST_ASSERT_EQUAL_UINT32(0, ring.completedBlocks());
    TEST_ASSERT_NULL(ring.block(0));

    queueBlocks(ring, 2, 3);
    TEST_ASSERT_EQUAL_UINT32(1, ring.completedBlocks());
    TEST_ASSERT_NOT_NULL(ring.block(0));
    TEST_ASSERT_NULL(ring.block(1));
}

void test_complete_blocks_are_handed_out_in_order(void) {
    BlockRing ring(kLen, kCount);
    queueBlocks(ring, 0, 5); // 0..2 complete, 3 and 4 in flight

    for (uint32_t seq = 0; seq < 3; ++seq) {
        const int16_t *b = ring.block(seq);
        TEST_ASSERT_NOT_NULL(b);
        TEST_ASSERT_EQUAL_INT16(seq * 10, b[0]);
        TEST_ASSERT_EQUAL_INT16(seq * 10 + 2, b[2]);
    }
}

// Queued 0..7: block 7 went into block 2's slot, so block 2 is gone, while
// block 3 (complete, in the slot the microphone uses next) is still there.
void test_a_block_whose_slot_was_reused_is_gone(void) {
    BlockRing ring(kLen, kCount);
    queueBlocks(ring, 0, 8);

    TEST_ASSERT_NULL(ring.block(2));
    const int16_t *b = ring.block(3);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_INT16(30, b[0]);
    TEST_ASSERT_EQUAL_INT16(50, ring.block(5)[0]);
    TEST_ASSERT_NULL(ring.block(6)); // still in flight
}

// After a microphone restart the old blocks are gone and counting starts
// over from the first slot.
void test_reset_forgets_every_block(void) {
    BlockRing ring(kLen, kCount);
    int16_t *first = ring.nextWriteBlock();
    queueBlocks(ring, 0, 4);

    ring.reset();
    TEST_ASSERT_EQUAL_UINT32(0, ring.completedBlocks());
    TEST_ASSERT_NULL(ring.block(0));
    TEST_ASSERT_EQUAL_PTR(first, ring.nextWriteBlock());

    queueBlocks(ring, 100, 103);
    TEST_ASSERT_EQUAL_INT16(1000, ring.block(0)[0]);
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_write_blocks_walk_the_ring_and_wrap);
    RUN_TEST(test_blocks_in_the_queue_are_not_complete);
    RUN_TEST(test_complete_blocks_are_handed_out_in_order);
    RUN_TEST(test_a_block_whose_slot_was_reused_is_gone);
    RUN_TEST(test_reset_forgets_every_block);

    return UNITY_END();
}
