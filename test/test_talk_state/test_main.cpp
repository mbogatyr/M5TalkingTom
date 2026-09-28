#include <unity.h>

#include "TalkState.h"

using In = TalkState::Input;

static In key1() {
    In in;
    in.key1 = true;
    return in;
}
static In started() {
    In in;
    in.phraseStarted = true;
    return in;
}
static In ended() {
    In in;
    in.phraseEnded = true;
    return in;
}
static In discarded() {
    In in;
    in.phraseDiscarded = true;
    return in;
}
static In processed() {
    In in;
    in.processed = true;
    return in;
}
static In playbackDone() {
    In in;
    in.playbackDone = true;
    return in;
}

// Hears a phrase at `at` and says it back, done 5 s later.
static void talk(TalkState &s, uint32_t at) {
    s.update(at, started());
    s.update(at + 2000, ended());
    s.update(at + 2300, processed());
    s.update(at + 5000, playbackDone());
}

void setUp(void) {}
void tearDown(void) {}

void test_starts_listening_as_the_cat(void) {
    TalkState s;
    s.begin(0);
    TEST_ASSERT_EQUAL(Mode::Listening, s.mode());
    TEST_ASSERT_EQUAL_UINT8(0, s.character());
    TEST_ASSERT_FALSE(s.hasSwitched());
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(10, In{}));
}

void test_key1_walks_cat_hippo_mouse_and_round(void) {
    TalkState s;
    s.begin(0);
    s.update(100, key1());
    TEST_ASSERT_EQUAL_UINT8(1, s.character());
    TEST_ASSERT_EQUAL_UINT8(0, s.previousCharacter());
    TEST_ASSERT_TRUE(s.hasSwitched());
    TEST_ASSERT_EQUAL_UINT32(100, s.switchedAtMs());
    s.update(200, key1());
    TEST_ASSERT_EQUAL_UINT8(2, s.character());
    s.update(300, key1());
    TEST_ASSERT_EQUAL_UINT8(0, s.character());
    TEST_ASSERT_EQUAL_UINT8(2, s.previousCharacter());
}

void test_a_phrase_goes_round_the_whole_cycle(void) {
    TalkState s;
    s.begin(0);
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(1000, started()));
    TEST_ASSERT_EQUAL(Mode::Hearing, s.mode());

    TEST_ASSERT_EQUAL_UINT8(TalkState::kStopMic | TalkState::kProcess, s.update(3000, ended()));
    TEST_ASSERT_EQUAL(Mode::Thinking, s.mode());

    TEST_ASSERT_EQUAL_UINT8(TalkState::kPlay, s.update(3300, processed()));
    TEST_ASSERT_EQUAL(Mode::Talking, s.mode());
    TEST_ASSERT_EQUAL_UINT32(3300, s.modeSinceMs());

    TEST_ASSERT_EQUAL_UINT8(TalkState::kStartMic, s.update(6000, playbackDone()));
    TEST_ASSERT_EQUAL(Mode::Listening, s.mode());
}

void test_a_dropped_click_goes_back_to_listening(void) {
    TalkState s;
    s.begin(0);
    s.update(1000, started());
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(1800, discarded()));
    TEST_ASSERT_EQUAL(Mode::Listening, s.mode());
}

void test_key1_while_hearing_switches_and_keeps_recording(void) {
    TalkState s;
    s.begin(0);
    s.update(1000, started());
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(1500, key1()));
    TEST_ASSERT_EQUAL(Mode::Hearing, s.mode());
    TEST_ASSERT_EQUAL_UINT8(1, s.character());
}

void test_key1_while_thinking_restarts_with_the_new_voice(void) {
    TalkState s;
    s.begin(0);
    s.update(1000, started());
    s.update(2000, ended());
    TEST_ASSERT_EQUAL_UINT8(TalkState::kProcess, s.update(2100, key1()));
    TEST_ASSERT_EQUAL(Mode::Thinking, s.mode());
    TEST_ASSERT_EQUAL_UINT8(1, s.character());
}

void test_key1_while_talking_stops_and_listens(void) {
    TalkState s;
    s.begin(0);
    s.update(1000, started());
    s.update(2000, ended());
    s.update(2200, processed());
    TEST_ASSERT_EQUAL_UINT8(TalkState::kStopPlayback | TalkState::kStartMic, s.update(3000, key1()));
    TEST_ASSERT_EQUAL(Mode::Listening, s.mode());
    TEST_ASSERT_EQUAL_UINT8(1, s.character());
}

void test_two_idle_minutes_lead_to_goodbye_and_power_off(void) {
    TalkState s;
    s.begin(0);
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(119999, In{}));
    TEST_ASSERT_EQUAL_UINT8(TalkState::kStopMic, s.update(120000, In{}));
    TEST_ASSERT_EQUAL(Mode::Goodbye, s.mode());

    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(122999, In{}));
    TEST_ASSERT_EQUAL_UINT8(TalkState::kPowerOff, s.update(123000, In{}));
    // Only once.
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(124000, In{}));
}

void test_it_gets_sleepy_for_the_last_20_seconds(void) {
    TalkState s;
    s.begin(0);
    s.update(99999, In{});
    TEST_ASSERT_FALSE(s.sleepy(99999));
    TEST_ASSERT_TRUE(s.sleepy(100000));
    TEST_ASSERT_EQUAL_UINT32(100000, s.idleFor(100000));
}

void test_a_phrase_restarts_the_idle_clock(void) {
    TalkState s;
    s.begin(0);
    talk(s, 100000); // back to listening at 105000
    TEST_ASSERT_FALSE(s.sleepy(110000));
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(224999, In{}));
    TEST_ASSERT_EQUAL_UINT8(TalkState::kStopMic, s.update(225000, In{}));
}

void test_key1_restarts_the_idle_clock(void) {
    TalkState s;
    s.begin(0);
    s.update(110000, key1());
    TEST_ASSERT_FALSE(s.sleepy(110000));
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(229999, In{}));
    TEST_ASSERT_EQUAL(Mode::Listening, s.mode());
}

// A click neither counts as a phrase nor eats the time it took.
void test_a_click_pauses_the_idle_clock(void) {
    TalkState s;
    s.begin(0);
    s.update(60000, started());
    s.update(61000, discarded());
    TEST_ASSERT_EQUAL_UINT32(60000, s.idleFor(61000));
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(120999, In{}));
    TEST_ASSERT_EQUAL_UINT8(TalkState::kStopMic, s.update(121000, In{}));
}

// Hearing, thinking and talking are not idleness, however long.
void test_the_idle_clock_stops_outside_listening(void) {
    TalkState s;
    s.begin(0);
    s.update(1000, started());
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(500000, In{}));
    TEST_ASSERT_EQUAL(Mode::Hearing, s.mode());
    TEST_ASSERT_EQUAL_UINT32(0, s.idleFor(500000));
    TEST_ASSERT_FALSE(s.sleepy(500000));
}

void test_key1_while_waving_keeps_the_toy_on(void) {
    TalkState s;
    s.begin(0);
    s.update(120000, In{});
    TEST_ASSERT_EQUAL_UINT8(TalkState::kStartMic, s.update(121000, key1()));
    TEST_ASSERT_EQUAL(Mode::Listening, s.mode());
    TEST_ASSERT_EQUAL_UINT8(0, s.character()); // no switch
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(123000, In{}));
    TEST_ASSERT_EQUAL_UINT8(TalkState::kStopMic, s.update(241000, In{}));
}

void test_a_shorter_idle_time_for_testing(void) {
    TalkState s;
    s.begin(0);
    s.setIdleTimeout(5000);
    TEST_ASSERT_TRUE(s.sleepy(0)); // shorter than the drowsy stretch
    TEST_ASSERT_EQUAL_UINT8(TalkState::kStopMic, s.update(5000, In{}));
}

void test_idle_time_0_keeps_the_toy_on(void) {
    TalkState s;
    s.begin(0);
    s.setIdleTimeout(0);
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(3600000, In{}));
    TEST_ASSERT_EQUAL(Mode::Listening, s.mode());
    TEST_ASSERT_FALSE(s.sleepy(3600000));
}

void test_idle_clock_survives_the_millis_rollover(void) {
    TalkState s;
    const uint32_t nearOverflow = 0xFFFFFF00u;
    s.begin(nearOverflow);
    TEST_ASSERT_EQUAL_UINT8(TalkState::kNone, s.update(nearOverflow + 100000, In{}));
    TEST_ASSERT_TRUE(s.sleepy(nearOverflow + 100000));
    TEST_ASSERT_EQUAL_UINT8(TalkState::kStopMic, s.update(nearOverflow + 120000, In{}));
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_starts_listening_as_the_cat);
    RUN_TEST(test_key1_walks_cat_hippo_mouse_and_round);
    RUN_TEST(test_a_phrase_goes_round_the_whole_cycle);
    RUN_TEST(test_a_dropped_click_goes_back_to_listening);
    RUN_TEST(test_key1_while_hearing_switches_and_keeps_recording);
    RUN_TEST(test_key1_while_thinking_restarts_with_the_new_voice);
    RUN_TEST(test_key1_while_talking_stops_and_listens);
    RUN_TEST(test_two_idle_minutes_lead_to_goodbye_and_power_off);
    RUN_TEST(test_it_gets_sleepy_for_the_last_20_seconds);
    RUN_TEST(test_a_phrase_restarts_the_idle_clock);
    RUN_TEST(test_key1_restarts_the_idle_clock);
    RUN_TEST(test_a_click_pauses_the_idle_clock);
    RUN_TEST(test_the_idle_clock_stops_outside_listening);
    RUN_TEST(test_key1_while_waving_keeps_the_toy_on);
    RUN_TEST(test_a_shorter_idle_time_for_testing);
    RUN_TEST(test_idle_time_0_keeps_the_toy_on);
    RUN_TEST(test_idle_clock_survives_the_millis_rollover);

    return UNITY_END();
}
