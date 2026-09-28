#pragma once

#include <M5Unified.h>

#include "TalkState.h"

// Everything a frame depends on.
struct Frame {
    uint32_t timeMs = 0;       // animation clock
    Mode mode = Mode::Listening;
    uint32_t modeMs = 0;       // time in this mode
    uint8_t character = 0;
    uint8_t previousCharacter = 0;
    bool switching = false;    // the switch animation is running
    uint32_t switchMs = 0;     // time since KEY1 switched the character
    uint8_t mouth = 0;         // 0 closed .. 255 wide open
    uint8_t micLevel = 0;      // 0 .. 255, how loud the phrase being heard is
    bool sleepy = false;
};

// Draws the characters on the built-in StickS3 display (135x240, portrait).
//
// The whole frame is composed in a sprite and pushed in a single call:
// drawing directly on the screen causes noticeable flicker.
class Renderer {
  public:
    static constexpr uint32_t kSwitchMs = 850;

    // Call after M5.begin().
    void begin();

    void draw(const Frame &f);

    // The last frame as "SNAP <w> <h>\n" plus RGB565, high byte first.
    void writeSnapshot(Print &out);

    uint32_t lastPaintUs() const { return paintUs_; }
    uint32_t lastPushUs() const { return pushUs_; }

  private:
    void paint(const Frame &f);

    M5Canvas canvas_{&M5.Display};
    uint32_t paintUs_ = 0;
    uint32_t pushUs_ = 0;
};
