#pragma once

#include <M5Unified.h>

// Draws on the built-in StickS3 display (135x240).
//
// The whole frame is composed in a sprite and pushed in a single call:
// drawing directly on the screen causes noticeable flicker.
//
// For now this is a placeholder that shows the time since power-on.
// In a new project the draw() arguments, the previous*_ fields and the
// body of paint() change; the sprite, invalidate() and skipping identical
// frames stay.
class Renderer {
  public:
    // Call after M5.begin().
    void begin();

    // Redraws the screen only when the picture has changed.
    void draw(uint32_t uptimeSeconds);

    // Forgets the last frame. Needed after the display wakes up: its
    // contents are lost, and otherwise the comparison with the previous
    // frame would decide there is nothing to redraw.
    void invalidate();

  private:
    void paint(uint32_t uptimeSeconds);

    M5Canvas canvas_{&M5.Display};

    bool hasPrevious_ = false;
    uint32_t previousSeconds_ = 0;
};
