#pragma once

#include <M5Unified.h>

#include "Painter.h"
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

// Draws the characters on the built-in StickS3 display (135x240, portrait,
// KEY1 below the screen).
//
// A port of tools/character_prototype.html: the same shapes, coordinates,
// colours, poses and timings. Change the look there first.
//
// The whole frame is composed in a sprite and pushed in a single call:
// drawing directly on the screen causes noticeable flicker.
class Renderer {
  public:
    // Old character drops out (250 ms), new one falls in and bounces.
    static constexpr uint32_t kSwitchMs = 850;
    // Goodbye: waving, then a fade to black.
    static constexpr uint32_t kWaveMs = 2400;
    static constexpr uint32_t kFadeMs = 600;

    // Call after M5.begin().
    void begin();

    void draw(const Frame &f);

    // The last frame as "SNAP <w> <h>\n" plus RGB565, high byte first.
    void writeSnapshot(Print &out);

    uint32_t lastPaintUs() const { return paintUs_; }
    uint32_t lastPushUs() const { return pushUs_; }

  private:
    enum class Look : uint8_t { Idle, Hear, Think, Talk, Sleepy, Bye };

    // Everything that moves, worked out from the look and the time.
    struct Pose {
        Look look = Look::Idle;
        float breath = 0, bob = 0, tilt = 0, mouth = 0, lid = 0;
        float lookX = 0, lookY = 0, earL = 0, earR = 0;
        float level = 0, hop = 0, gL = 0, gR = 0, wave = 0;
    };
    // Where the overlays go: the ear the sound waves come to, the top of
    // the head the "z" float from.
    struct Anchors {
        float earX, earY, headX, headY;
    };

    void paint(const Frame &f);
    Pose pose(uint8_t character, Look look, float t, const Frame &f) const;
    void eye(float x, float y, float rx, float ry, uint16_t iris, float ir, float prx, float pry,
             uint16_t lidColour, float lid, const Pose &p);
    Anchors drawCat(const Pose &p, float t);
    Anchors drawHippo(const Pose &p, float t);
    Anchors drawMouse(const Pose &p, float t);
    void background(uint8_t character, float t);
    void foreground(uint8_t character, float t);
    void overlays(Look look, const Anchors &a, const Pose &p, float t);
    void topBar(uint8_t character);

    M5Canvas canvas_{&M5.Display};
    Painter p_;
    uint32_t paintUs_ = 0;
    uint32_t pushUs_ = 0;
};
