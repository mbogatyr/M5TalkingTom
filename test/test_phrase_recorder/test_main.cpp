#include <unity.h>

#include <math.h>

#include <vector>

#include "PhraseRecorder.h"

// Default config: 16 kHz, 512-sample blocks (32 ms). Numbers of blocks in
// the comments: pre-roll 320 ms = 10 blocks + the 2 blocks of the start
// run = 12; hang 700 ms = 22 blocks; tail 150 ms = 2400 samples; settle
// 250 ms = 8 blocks; min 350 ms; max 8000 ms.

static const size_t kLen = 512;
static const double kPi = 3.14159265358979;

struct Feed {
    PhraseRecorder::Config cfg;
    std::vector<int16_t> buffer;
    PhraseRecorder rec;
    int16_t block[kLen];
    uint32_t noiseSeed = 12345;
    double phase = 0;
    uint32_t blocks = 0; // blocks pushed so far
    std::vector<PhraseRecorder::Event> events;
    std::vector<uint32_t> eventBlocks;

    Feed()
        : buffer(PhraseRecorder::capacityFor(cfg)),
          rec(buffer.data(), buffer.size(), cfg) {}

    void push() {
        const PhraseRecorder::Event e = rec.push(block);
        if (e != PhraseRecorder::Event::None) {
            events.push_back(e);
            eventBlocks.push_back(blocks);
        }
        ++blocks;
    }

    // Uniform noise with the given RMS: a uniform value in [-a, a] has an
    // RMS of a / sqrt(3).
    void noise(double dbfs, uint32_t count) {
        const double a = 32768.0 * pow(10.0, dbfs / 20.0) * sqrt(3.0);
        for (uint32_t b = 0; b < count; ++b) {
            for (size_t i = 0; i < kLen; ++i) {
                noiseSeed = noiseSeed * 1664525u + 1013904223u;
                const double u = (noiseSeed >> 8) / 16777216.0 * 2.0 - 1.0;
                block[i] = static_cast<int16_t>(u * a);
            }
            push();
        }
    }

    // A 220 Hz tone standing in for a voice; its RMS is amplitude / sqrt(2).
    void voice(double dbfs, uint32_t count) {
        const double amp = 32768.0 * pow(10.0, dbfs / 20.0) * sqrt(2.0);
        for (uint32_t b = 0; b < count; ++b) {
            for (size_t i = 0; i < kLen; ++i) {
                block[i] = static_cast<int16_t>(amp * sin(phase));
                phase += 2 * kPi * 220.0 / 16000.0;
            }
            push();
        }
    }

    void zeros(uint32_t count) {
        for (uint32_t b = 0; b < count; ++b) {
            for (size_t i = 0; i < kLen; ++i) {
                block[i] = 0;
            }
            push();
        }
    }
};

void setUp(void) {}
void tearDown(void) {}

void test_block_level_of_a_full_scale_tone_is_minus_3_dbfs(void) {
    int16_t b[kLen];
    for (size_t i = 0; i < kLen; ++i) {
        b[i] = static_cast<int16_t>(32767 * sin(2 * kPi * i / 32.0));
    }
    TEST_ASSERT_FLOAT_WITHIN(0.1f, -3.01f, PhraseRecorder::blockDbfs(b, kLen));
}

void test_background_noise_never_starts_a_phrase(void) {
    Feed f;
    f.noise(-60, 320); // about 10 s
    TEST_ASSERT_EQUAL(0, f.events.size());
    TEST_ASSERT_FLOAT_WITHIN(4.0f, -60.0f, f.rec.noiseFloor());
}

void test_a_phrase_starts_and_ends_after_the_quiet(void) {
    Feed f;
    f.noise(-60, 30);  // blocks 0..29
    f.voice(-25, 47);  // blocks 30..76, 1.5 s
    f.noise(-60, 40);

    TEST_ASSERT_EQUAL(2, f.events.size());
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Started, f.events[0]);
    TEST_ASSERT_EQUAL_UINT32(31, f.eventBlocks[0]); // second loud block
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Ended, f.events[1]);
    // 22 quiet blocks after the last loud one (76): block 98.
    TEST_ASSERT_EQUAL_UINT32(98, f.eventBlocks[1]);

    // 10 blocks of pre-roll, 47 of voice, 2400 samples of tail.
    TEST_ASSERT_EQUAL_UINT32((10 + 47) * kLen + 2400, f.rec.length());
    TEST_ASSERT_TRUE(f.rec.holding());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -25.0f + 3.01f, f.rec.peakDbfs());
}

void test_pauses_between_words_keep_one_phrase(void) {
    Feed f;
    f.noise(-60, 30);
    f.voice(-25, 16);  // 0.5 s
    f.noise(-60, 12);  // 0.4 s pause
    f.voice(-25, 16);
    f.noise(-60, 40);

    TEST_ASSERT_EQUAL(2, f.events.size());
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Ended, f.events[1]);
    TEST_ASSERT_EQUAL_UINT32((10 + 16 + 12 + 16) * kLen + 2400, f.rec.length());
}

void test_a_click_is_discarded_and_listening_goes_on(void) {
    Feed f;
    f.noise(-60, 30);
    f.voice(-20, 3); // 96 ms
    f.noise(-60, 40);

    TEST_ASSERT_EQUAL(2, f.events.size());
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Started, f.events[0]);
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Discarded, f.events[1]);
    TEST_ASSERT_FALSE(f.rec.inPhrase());

    f.voice(-25, 20);
    f.noise(-60, 40);
    TEST_ASSERT_EQUAL(4, f.events.size());
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Ended, f.events[3]);
}

void test_a_long_monologue_is_cut_at_the_maximum(void) {
    Feed f;
    f.noise(-60, 30);
    f.voice(-25, 400); // 12.8 s

    TEST_ASSERT_EQUAL(2, f.events.size());
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Ended, f.events[1]);
    const uint32_t ms = f.rec.lengthMs();
    TEST_ASSERT_TRUE(ms >= 8000);
    TEST_ASSERT_TRUE(ms <= 8000 + 320 + 64);
    TEST_ASSERT_TRUE(f.rec.length() <= f.buffer.size());
}

void test_a_finished_phrase_is_held_until_reset(void) {
    Feed f;
    f.noise(-60, 30);
    f.voice(-25, 20);
    f.noise(-60, 30);
    const size_t len = f.rec.length();

    f.voice(-25, 30); // ignored: the phrase is being used
    TEST_ASSERT_EQUAL(2, f.events.size());
    TEST_ASSERT_EQUAL(len, f.rec.length());

    f.rec.reset();
    f.noise(-60, 20);
    f.voice(-25, 20);
    f.noise(-60, 30);
    TEST_ASSERT_EQUAL(4, f.events.size());
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Ended, f.events[3]);
}

// The codec gives zeros after the microphone restarts: they must neither
// start a phrase nor drag the noise estimate down to nothing.
void test_warm_up_zeros_are_ignored(void) {
    Feed f;
    f.noise(-55, 30);
    f.rec.reset();
    f.zeros(30);
    TEST_ASSERT_EQUAL(0, f.events.size());
    TEST_ASSERT_FLOAT_WITHIN(4.0f, -55.0f, f.rec.noiseFloor());

    f.noise(-55, 20);
    f.voice(-30, 20);
    f.noise(-55, 30);
    TEST_ASSERT_EQUAL(2, f.events.size());
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Ended, f.events[1]);
}

// Right after a restart the first 8 blocks only feed the noise estimate.
void test_nothing_starts_while_settling(void) {
    Feed f;
    f.noise(-60, 30);
    f.rec.reset();
    const uint32_t from = f.blocks;
    f.voice(-25, 20);
    TEST_ASSERT_EQUAL(1, f.events.size());
    // Settling eats 8 blocks, then two loud blocks start the phrase.
    TEST_ASSERT_EQUAL_UINT32(from + 9, f.eventBlocks[0]);
}

// In a loud room the threshold follows the noise up.
void test_loud_background_raises_the_threshold(void) {
    Feed f;
    f.noise(-40, 60);
    TEST_ASSERT_EQUAL(0, f.events.size());
    TEST_ASSERT_FLOAT_WITHIN(4.0f, -40.0f, f.rec.noiseFloor());

    f.voice(-30, 20); // only 10 dB above the noise
    TEST_ASSERT_EQUAL(0, f.events.size());

    f.noise(-40, 30);
    f.voice(-18, 20);
    f.noise(-40, 30);
    TEST_ASSERT_EQUAL(2, f.events.size());
    TEST_ASSERT_EQUAL(PhraseRecorder::Event::Ended, f.events[1]);
}

// A quiet room does not make the threshold hair-trigger: it never goes
// below -45 dBFS.
void test_threshold_has_an_absolute_floor(void) {
    Feed f;
    f.noise(-80, 60);
    f.voice(-50, 20); // 30 dB above the noise, but under -45 dBFS
    TEST_ASSERT_EQUAL(0, f.events.size());
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_block_level_of_a_full_scale_tone_is_minus_3_dbfs);
    RUN_TEST(test_background_noise_never_starts_a_phrase);
    RUN_TEST(test_a_phrase_starts_and_ends_after_the_quiet);
    RUN_TEST(test_pauses_between_words_keep_one_phrase);
    RUN_TEST(test_a_click_is_discarded_and_listening_goes_on);
    RUN_TEST(test_a_long_monologue_is_cut_at_the_maximum);
    RUN_TEST(test_a_finished_phrase_is_held_until_reset);
    RUN_TEST(test_warm_up_zeros_are_ignored);
    RUN_TEST(test_nothing_starts_while_settling);
    RUN_TEST(test_loud_background_raises_the_threshold);
    RUN_TEST(test_threshold_has_an_absolute_floor);

    return UNITY_END();
}
