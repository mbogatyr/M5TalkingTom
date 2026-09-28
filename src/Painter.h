#pragma once

#include <stdint.h>

// RGB565 from 0xRRGGBB, so colours can be copied from the prototype.
constexpr uint16_t rgb(uint32_t hex) {
    return static_cast<uint16_t>(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F));
}

// Anti-aliased cartoon shapes straight into a 16-bit sprite buffer.
//
// This is the C++ side of tools/character_prototype.html: the same shape
// calls with the same coordinates. LovyanGFX fills ellipses and triangles
// without anti-aliasing, so the shapes are drawn here, pixel coverage
// taken from the distance to the edge.
//
// - part() runs a group of shapes twice: first grown by 2 px in the ink
//   colour, then filled. That gives the cartoon outline. Outlined shapes
//   (e, c, t, l, rr) take part in both passes, flat ones (fe, fc, ft, fl)
//   only in the fill pass.
// - rotate() turns the anchor points around a pivot (a head tilting on its
//   neck); the shapes themselves stay upright, like in the prototype.
// - setTransform() moves and squashes everything around the floor line,
//   for the bounce when a character drops in.
//
// The buffer keeps pixels byte-swapped (high byte first), the way a
// LovyanGFX sprite does for SPI.
class Painter {
  public:
    static constexpr float kCx = 67.0f;
    static constexpr float kFloor = 222.0f;
    static constexpr uint16_t kInk = rgb(0x2A2230);

    void begin(uint16_t *buffer, int width, int height);

    // Everything the character is made of.
    void setTransform(float dy, float sx, float sy);
    void resetTransform();
    void rotate(float px, float py, float angle); // 0 turns it off

    template <typename F> void part(F draw, float outline = 2.0f) {
        grow_ = outline;
        outlinePass_ = true;
        draw();
        outlinePass_ = false;
        draw();
        grow_ = 2.0f;
    }

    void e(float x, float y, float rx, float ry, uint16_t c) { ellipse(x, y, rx, ry, c, true); }
    void c(float x, float y, float r, uint16_t col) { ellipse(x, y, r, r, col, true); }
    void t(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) {
        triangle(x0, y0, x1, y1, x2, y2, c, true);
    }
    void l(float x0, float y0, float x1, float y1, float r, uint16_t c) { line(x0, y0, x1, y1, r, c, true); }
    void rr(float x, float y, float w, float h, float r, uint16_t c);

    void fe(float x, float y, float rx, float ry, uint16_t c) { ellipse(x, y, rx, ry, c, false); }
    void fc(float x, float y, float r, uint16_t col) { ellipse(x, y, r, r, col, false); }
    void ft(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) {
        triangle(x0, y0, x1, y1, x2, y2, c, false);
    }
    void fl(float x0, float y0, float x1, float y1, float r, uint16_t c) { line(x0, y0, x1, y1, r, c, false); }

    // An eyelid over the eye ellipse: f = 0 open, 1 closed.
    void lid(float x, float y, float rx, float ry, float f, uint16_t c);

    // Screen coordinates, no transform: backgrounds and overlays.
    void rect(int x, int y, int w, int h, uint16_t c);
    void ellipseAt(float x, float y, float rx, float ry, uint16_t c);
    void triangleAt(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c);
    void lineAt(float x0, float y0, float x1, float y1, float r, uint16_t c);
    void roundRectAt(float x, float y, float w, float h, float r, uint16_t c);
    // An arc of a ring: radius r, thickness 2*half, from angle a0 to a1
    // (radians, 0 = right, clockwise on screen).
    void arcAt(float x, float y, float r, float half, float a0, float a1, uint16_t c);

    // Scales every pixel toward black: 255 keeps it, 0 is black.
    void fade(uint8_t level);

  private:
    void map(float &x, float &y) const;
    float scaleX() const { return sx_; }
    float scaleY() const { return sy_; }
    bool skip(bool outlined) const { return outlinePass_ && !outlined; }
    uint16_t colour(uint16_t c) const { return outlinePass_ ? kInk : c; }
    float grown() const { return outlinePass_ ? grow_ : 0.0f; }

    void ellipse(float x, float y, float rx, float ry, uint16_t c, bool outlined);
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c, bool outlined);
    void line(float x0, float y0, float x1, float y1, float r, uint16_t c, bool outlined);

    void span(int y, int x0, int x1, uint16_t c);
    void blend(int x, int y, uint16_t c, float coverage);

    uint16_t *buf_ = nullptr;
    int w_ = 0;
    int h_ = 0;
    // Clip rectangle, [x0, x1) x [y0, y1).
    int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;

    float dy_ = 0, sx_ = 1, sy_ = 1;
    bool rotating_ = false;
    float px_ = 0, py_ = 0, cos_ = 1, sin_ = 0;

    bool outlinePass_ = false;
    float grow_ = 2.0f;
};
