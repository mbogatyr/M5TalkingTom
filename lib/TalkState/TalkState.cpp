#include "TalkState.h"

void TalkState::begin(uint32_t nowMs) {
    mode_ = Mode::Listening;
    modeSince_ = nowMs;
    lastActivity_ = nowMs;
    powerOffSent_ = false;
}

void TalkState::enter(Mode mode, uint32_t nowMs) {
    mode_ = mode;
    modeSince_ = nowMs;
}

void TalkState::nextCharacter(uint32_t nowMs) {
    previous_ = character_;
    character_ = static_cast<uint8_t>((character_ + 1) % kCharacters);
    switched_ = true;
    switchedAt_ = nowMs;
}

uint8_t TalkState::update(uint32_t nowMs, const Input &in) {
    uint8_t actions = kNone;

    switch (mode_) {
    case Mode::Listening:
        if (in.key1) {
            nextCharacter(nowMs);
            lastActivity_ = nowMs;
        }
        if (in.phraseStarted) {
            idleBeforeHearing_ = nowMs - lastActivity_;
            enter(Mode::Hearing, nowMs);
        } else if (idleMs_ > 0 && nowMs - lastActivity_ >= idleMs_) {
            enter(Mode::Goodbye, nowMs);
            powerOffSent_ = false;
            actions |= kStopMic;
        }
        break;

    case Mode::Hearing:
        // The recording goes on; the new character will repeat it.
        if (in.key1) {
            nextCharacter(nowMs);
        }
        if (in.phraseEnded) {
            enter(Mode::Thinking, nowMs);
            actions |= kStopMic | kProcess;
        } else if (in.phraseDiscarded) {
            // Not a phrase: the idle clock picks up where it stopped.
            lastActivity_ = nowMs - idleBeforeHearing_;
            enter(Mode::Listening, nowMs);
        }
        break;

    case Mode::Thinking:
        if (in.key1) {
            nextCharacter(nowMs);
            actions |= kProcess;
        } else if (in.processed) {
            enter(Mode::Talking, nowMs);
            actions |= kPlay;
        }
        break;

    case Mode::Talking:
        if (in.key1) {
            nextCharacter(nowMs);
            actions |= kStopPlayback | kStartMic;
            lastActivity_ = nowMs;
            enter(Mode::Listening, nowMs);
        } else if (in.playbackDone) {
            actions |= kStartMic;
            lastActivity_ = nowMs;
            enter(Mode::Listening, nowMs);
        }
        break;

    case Mode::Goodbye:
        // A press while it waves keeps the toy on.
        if (in.key1) {
            actions |= kStartMic;
            lastActivity_ = nowMs;
            enter(Mode::Listening, nowMs);
        } else if (!powerOffSent_ && nowMs - modeSince_ >= kGoodbyeMs) {
            powerOffSent_ = true;
            actions |= kPowerOff;
        }
        break;
    }
    return actions;
}

uint32_t TalkState::idleFor(uint32_t nowMs) const {
    return mode_ == Mode::Listening ? nowMs - lastActivity_ : 0;
}

bool TalkState::sleepy(uint32_t nowMs) const {
    if (mode_ != Mode::Listening || idleMs_ == 0) {
        return false;
    }
    const uint32_t from = idleMs_ > kSleepyForMs ? idleMs_ - kSleepyForMs : 0;
    return idleFor(nowMs) >= from;
}
