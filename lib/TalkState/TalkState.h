#pragma once

#include <stdint.h>

// What the toy is doing.
enum class Mode : uint8_t {
    Listening, // mic on, waiting for a phrase
    Hearing,   // a phrase is being recorded
    Thinking,  // the voice is being changed
    Talking,   // the character repeats the phrase
    Goodbye,   // two minutes without a phrase: waving, then power off
};

// The toy's state machine: which mode, which character, and when to give
// up and power off.
//
// Events come in (KEY1, the phrase recorder's news, the voice changer and
// the speaker being done), actions for main.cpp come out. It does not touch
// the hardware; time is passed in.
//
// The idle clock only runs while listening: a phrase being heard, changed
// or said is not idleness. A finished playback and a KEY1 press restart
// it; a phrase dropped as too short does not.
class TalkState {
  public:
    static constexpr uint8_t kCharacters = 3;
    static constexpr uint32_t kIdleMs = 120000;     // 2 minutes
    static constexpr uint32_t kSleepyForMs = 20000; // drowsy for the last 20 s
    static constexpr uint32_t kGoodbyeMs = 3000;    // waving before power off

    struct Input {
        bool key1 = false;
        bool phraseStarted = false;
        bool phraseEnded = false;
        bool phraseDiscarded = false;
        bool processed = false;    // the voice changer is done
        bool playbackDone = false; // the speaker has finished
    };

    // Bits of the value update() returns.
    enum Action : uint8_t {
        kNone = 0,
        kStopMic = 1 << 0,
        kStartMic = 1 << 1,
        kProcess = 1 << 2, // (re)start changing the recorded phrase
        kPlay = 1 << 3,
        kStopPlayback = 1 << 4,
        kPowerOff = 1 << 5,
    };

    void begin(uint32_t nowMs);
    uint8_t update(uint32_t nowMs, const Input &in);

    // For tests on the board: a shorter idle time.
    void setIdleTimeout(uint32_t ms) { idleMs_ = ms; }
    uint32_t idleTimeout() const { return idleMs_; }

    Mode mode() const { return mode_; }
    uint32_t modeSinceMs() const { return modeSince_; }

    uint8_t character() const { return character_; }
    uint8_t previousCharacter() const { return previous_; }
    // When KEY1 last switched the character; false before the first switch.
    bool hasSwitched() const { return switched_; }
    uint32_t switchedAtMs() const { return switchedAt_; }

    // Time without activity; 0 unless listening.
    uint32_t idleFor(uint32_t nowMs) const;
    bool sleepy(uint32_t nowMs) const;

  private:
    void enter(Mode mode, uint32_t nowMs);
    void nextCharacter(uint32_t nowMs);

    Mode mode_ = Mode::Listening;
    uint32_t modeSince_ = 0;
    uint32_t lastActivity_ = 0;
    uint32_t idleBeforeHearing_ = 0;
    uint32_t idleMs_ = kIdleMs;
    bool powerOffSent_ = false;

    uint8_t character_ = 0;
    uint8_t previous_ = 0;
    bool switched_ = false;
    uint32_t switchedAt_ = 0;
};
