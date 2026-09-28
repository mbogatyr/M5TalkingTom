#include <unity.h>

#include <math.h>

#include <vector>

#include "LipSync.h"

// 16 kHz: a 20 ms frame is 320 samples.
static const uint32_t kRate = 16000;

static void tone(std::vector<int16_t> &v, size_t from, size_t to, double amp) {
    for (size_t i = from; i < to; ++i) {
        v[i] = static_cast<int16_t>(amp * sin(2 * 3.14159265 * 300.0 * i / kRate));
    }
}

void setUp(void) {}
void tearDown(void) {}

void test_silence_keeps_the_mouth_shut(void) {
    std::vector<int16_t> v(16000, 0);
    LipSync lips;
    lips.analyze(v.data(), v.size(), kRate);
    TEST_ASSERT_EQUAL(50, lips.frames());
    for (uint32_t ms = 0; ms < 1000; ms += 7) {
        TEST_ASSERT_EQUAL_UINT8(0, lips.openness(ms));
    }
}

// A loud tone (-7 dBFS RMS) opens the mouth wide from its first frame.
void test_a_loud_sound_opens_it_wide_at_once(void) {
    std::vector<int16_t> v(16000, 0);
    tone(v, 3200, 9600, 20000); // 200..600 ms
    LipSync lips;
    lips.analyze(v.data(), v.size(), kRate);
    TEST_ASSERT_EQUAL_UINT8(0, lips.openness(150));
    TEST_ASSERT_EQUAL_UINT8(255, lips.openness(200));
    TEST_ASSERT_EQUAL_UINT8(255, lips.openness(400));
}

// After the sound it closes by 70 per frame: 255, 185, 115, 45, 0.
void test_it_closes_over_a_few_frames(void) {
    std::vector<int16_t> v(16000, 0);
    tone(v, 3200, 9600, 20000);
    LipSync lips;
    lips.analyze(v.data(), v.size(), kRate);
    TEST_ASSERT_EQUAL_UINT8(185, lips.openness(600));
    TEST_ASSERT_EQUAL_UINT8(115, lips.openness(620));
    TEST_ASSERT_EQUAL_UINT8(45, lips.openness(640));
    TEST_ASSERT_EQUAL_UINT8(0, lips.openness(660));
}

// -26 dBFS RMS is halfway between -40 and -12.
void test_a_middling_sound_opens_it_halfway(void) {
    std::vector<int16_t> v(16000, 0);
    const double amp = 32768.0 * pow(10.0, -26.0 / 20.0) * sqrt(2.0);
    tone(v, 0, 16000, amp);
    LipSync lips;
    lips.analyze(v.data(), v.size(), kRate);
    TEST_ASSERT_UINT8_WITHIN(4, 128, lips.openness(500));
}

void test_between_frames_it_moves_in_a_straight_line(void) {
    std::vector<int16_t> v(16000, 0);
    tone(v, 3200, 9600, 20000);
    LipSync lips;
    lips.analyze(v.data(), v.size(), kRate);
    // Frame 30 is 185, frame 31 is 115: halfway is 150.
    TEST_ASSERT_EQUAL_UINT8(150, lips.openness(610));
}

void test_after_the_end_the_mouth_is_shut(void) {
    std::vector<int16_t> v(3200, 20000);
    LipSync lips;
    lips.analyze(v.data(), v.size(), kRate);
    TEST_ASSERT_EQUAL(10, lips.frames());
    TEST_ASSERT_EQUAL_UINT8(0, lips.openness(200));
    TEST_ASSERT_EQUAL_UINT8(0, lips.openness(100000));
}

void test_very_long_phrases_are_capped(void) {
    std::vector<int16_t> v(kRate * 25, 0);
    LipSync lips;
    lips.analyze(v.data(), v.size(), kRate);
    TEST_ASSERT_EQUAL(LipSync::kMaxFrames, lips.frames());
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_silence_keeps_the_mouth_shut);
    RUN_TEST(test_a_loud_sound_opens_it_wide_at_once);
    RUN_TEST(test_it_closes_over_a_few_frames);
    RUN_TEST(test_a_middling_sound_opens_it_halfway);
    RUN_TEST(test_between_frames_it_moves_in_a_straight_line);
    RUN_TEST(test_after_the_end_the_mouth_is_shut);
    RUN_TEST(test_very_long_phrases_are_capped);

    return UNITY_END();
}
