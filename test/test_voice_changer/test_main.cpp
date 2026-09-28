#include <unity.h>

#include <math.h>
#include <stdlib.h>

#include <vector>

#include "VoiceChanger.h"

static const double kPi = 3.14159265358979;
static const double kRate = 16000.0;

static std::vector<int16_t> sine(double hz, double amp, size_t n) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<int16_t>(lrint(amp * sin(2 * kPi * hz * i / kRate)));
    }
    return v;
}

struct Result {
    std::vector<int16_t> scratch;
    std::vector<int16_t> out;
    size_t len = 0;
};

static Result process(const std::vector<int16_t> &in, const Voice &v, size_t work = 1u << 30) {
    Result r;
    r.scratch.resize(VoiceChanger::stretchCapacity(in.size(), v));
    r.out.resize(VoiceChanger::outputCapacity(in.size(), v));
    VoiceChanger vc;
    vc.start(in.data(), in.size(), r.scratch.data(), r.scratch.size(), r.out.data(),
             r.out.size(), v);
    while (!vc.step(work)) {
    }
    r.len = vc.length();
    return r;
}

// Mean frequency from rising zero crossings between `from` and `to`,
// with the crossing points interpolated between samples.
static double frequency(const std::vector<int16_t> &y, size_t from, size_t to) {
    double first = -1, last = -1;
    int count = 0;
    for (size_t i = from + 1; i < to; ++i) {
        if (y[i - 1] < 0 && y[i] >= 0) {
            const double t = (i - 1) + static_cast<double>(-y[i - 1]) / (y[i] - y[i - 1]);
            if (first < 0) {
                first = t;
            }
            last = t;
            ++count;
        }
    }
    if (count < 2) {
        return 0;
    }
    return (count - 1) / ((last - first) / kRate);
}

static int32_t peakOf(const std::vector<int16_t> &y, size_t len) {
    int32_t p = 0;
    for (size_t i = 0; i < len; ++i) {
        p = std::max(p, static_cast<int32_t>(abs(y[i])));
    }
    return p;
}

static Voice clean(const Voice &v) { return Voice{v.pitch, v.tempo, 0.0f, 0.0f, 0.0f}; }

void setUp(void) {}
void tearDown(void) {}

// The output lasts input / tempo, give or take a window.
void test_output_length_follows_the_tempo(void) {
    const std::vector<int16_t> in = sine(200, 12000, 32000); // 2 s
    const Voice all[] = {voices::kCat, voices::kHippo, voices::kMouse};
    for (const Voice &v : all) {
        const Result r = process(in, v);
        const double expected = in.size() / v.tempo;
        TEST_ASSERT_FLOAT_WITHIN(2.0f * VoiceChanger::kFrame, static_cast<float>(expected), static_cast<float>(r.len));
    }
}

// 200 Hz comes out at 200 * pitch: 320, 124 and 440 Hz.
void test_pitch_moves_by_the_voice_ratio(void) {
    const std::vector<int16_t> in = sine(200, 12000, 24000);
    const Voice all[] = {voices::kCat, voices::kHippo, voices::kMouse};
    for (const Voice &v : all) {
        const Result r = process(in, clean(v));
        const double f = frequency(r.out, 2000, r.len - 2000);
        TEST_ASSERT_FLOAT_WITHIN(200 * v.pitch * 0.02f, 200 * v.pitch, static_cast<float>(f));
    }
}

// Vibrato wobbles the pitch around the same mean.
void test_vibrato_keeps_the_mean_pitch(void) {
    const std::vector<int16_t> in = sine(200, 12000, 32000);
    const Result r = process(in, voices::kMouse);
    const double f = frequency(r.out, 2000, r.len - 2000);
    TEST_ASSERT_FLOAT_WITHIN(200 * voices::kMouse.pitch * 0.03f, 200 * voices::kMouse.pitch, static_cast<float>(f));
}

// WSOLA splices windows where the waveforms line up: a clean tone stays
// clean, with no step much bigger than the tone's own steepest slope. The
// limiter flattens the tops, which makes the slope near zero up to about
// 1.3 times a pure sine's of the same peak; a bad splice would jump by up
// to twice the peak.
void test_splices_leave_no_clicks(void) {
    const std::vector<int16_t> in = sine(200, 12000, 24000);
    const Voice all[] = {voices::kCat, voices::kHippo, voices::kMouse};
    for (const Voice &v : all) {
        const Result r = process(in, clean(v));
        const double amp = peakOf(r.out, r.len);
        const double slope = amp * 2 * kPi * 200 * v.pitch / kRate;
        int32_t worst = 0;
        for (size_t i = 1; i < r.len; ++i) {
            worst = std::max(worst, static_cast<int32_t>(abs(r.out[i] - r.out[i - 1])));
        }
        TEST_ASSERT_TRUE_MESSAGE(worst <= 1.6 * slope, "a click in the output");
    }
}

// A loud phrase peaks at -1 dBFS (29204).
void test_output_is_normalised_to_minus_1_dbfs(void) {
    const Result r = process(sine(200, 20000, 24000), voices::kCat);
    const int32_t peak = peakOf(r.out, r.len);
    TEST_ASSERT_INT_WITHIN(400, 29000, peak);
    TEST_ASSERT_TRUE(peak <= 29204);
}

// A quiet one gets at most +20 dB, so hiss stays hiss.
void test_gain_is_capped_at_20_db(void) {
    const Result r = process(sine(200, 300, 24000), clean(voices::kCat));
    const int32_t peak = peakOf(r.out, r.len);
    TEST_ASSERT_INT_WITHIN(300, 3000, peak);
}

// The limiter lifts quiet syllables: a phrase whose second half is 12 dB
// quieter comes out with that half well above a plain peak normalisation
// (which would leave it at 29204 / 4 = 7301).
void test_quiet_syllables_come_up(void) {
    std::vector<int16_t> in = sine(200, 20000, 32000);
    for (size_t i = 16000; i < in.size(); ++i) {
        in[i] /= 4;
    }
    const Result r = process(in, clean(voices::kCat));
    const size_t half = r.len / 2;
    int32_t quietPeak = 0;
    for (size_t i = half + 2000; i < r.len - 2000; ++i) {
        quietPeak = std::max(quietPeak, static_cast<int32_t>(abs(r.out[i])));
    }
    TEST_ASSERT_TRUE(quietPeak > 1.8 * 7301);
    TEST_ASSERT_TRUE(peakOf(r.out, r.len) <= 29204);
}

void test_silence_stays_silent(void) {
    const std::vector<int16_t> in(24000, 0);
    const Result r = process(in, voices::kHippo);
    TEST_ASSERT_TRUE(r.len > 0);
    TEST_ASSERT_EQUAL_INT32(0, peakOf(r.out, r.len));
}

// Working in small steps gives exactly the same output as in one go.
void test_small_steps_give_the_same_output(void) {
    const std::vector<int16_t> in = sine(310, 9000, 20000);
    const Result whole = process(in, voices::kMouse);
    const Result steps = process(in, voices::kMouse, 1);
    TEST_ASSERT_EQUAL(whole.len, steps.len);
    TEST_ASSERT_EQUAL_INT16_ARRAY(whole.out.data(), steps.out.data(), whole.len);
}

void test_a_too_short_input_gives_nothing(void) {
    const std::vector<int16_t> in = sine(200, 9000, 600);
    const Result r = process(in, voices::kCat);
    TEST_ASSERT_EQUAL(0, r.len);
}

// The hippo's soft clipping squares the wave off: its RMS gets closer to
// its peak than a sine's 0.707.
void test_drive_squares_off_the_hippo(void) {
    const std::vector<int16_t> in = sine(200, 12000, 24000);
    const Result r = process(in, voices::kHippo);
    double sum = 0;
    for (size_t i = 2000; i < r.len - 2000; ++i) {
        sum += static_cast<double>(r.out[i]) * r.out[i];
    }
    const double rms = sqrt(sum / (r.len - 4000));
    TEST_ASSERT_TRUE(rms / peakOf(r.out, r.len) > 0.8);
}

// Raising the pitch 2.2 times would push 5 kHz to 11 kHz, past the 8 kHz
// Nyquist frequency, and it would fold back as 5 kHz noise; the low-pass
// takes it out first. Quiet inputs so that both get the same capped gain.
void test_tones_that_would_fold_over_are_filtered_out(void) {
    const Voice v = clean(voices::kMouse);
    const Result low = process(sine(1000, 2000, 24000), v);
    const Result high = process(sine(5000, 2000, 24000), v);
    TEST_ASSERT_TRUE(peakOf(high.out, high.len) * 4 < peakOf(low.out, low.len));
}

void test_characters_have_their_voices(void) {
    TEST_ASSERT_EQUAL_FLOAT(voices::kCat.pitch, voices::forCharacter(0).pitch);
    TEST_ASSERT_EQUAL_FLOAT(voices::kHippo.pitch, voices::forCharacter(1).pitch);
    TEST_ASSERT_EQUAL_FLOAT(voices::kMouse.pitch, voices::forCharacter(2).pitch);
    TEST_ASSERT_TRUE(voices::kHippo.pitch < 1.0f);
    TEST_ASSERT_TRUE(voices::kMouse.pitch > voices::kCat.pitch);
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_output_length_follows_the_tempo);
    RUN_TEST(test_pitch_moves_by_the_voice_ratio);
    RUN_TEST(test_vibrato_keeps_the_mean_pitch);
    RUN_TEST(test_splices_leave_no_clicks);
    RUN_TEST(test_output_is_normalised_to_minus_1_dbfs);
    RUN_TEST(test_gain_is_capped_at_20_db);
    RUN_TEST(test_quiet_syllables_come_up);
    RUN_TEST(test_silence_stays_silent);
    RUN_TEST(test_small_steps_give_the_same_output);
    RUN_TEST(test_a_too_short_input_gives_nothing);
    RUN_TEST(test_drive_squares_off_the_hippo);
    RUN_TEST(test_tones_that_would_fold_over_are_filtered_out);
    RUN_TEST(test_characters_have_their_voices);

    return UNITY_END();
}
