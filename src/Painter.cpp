// Every frame goes through these loops; -O2 makes them about twice as fast
// as the core's -Os.
#pragma GCC optimize("O2")

#include "Painter.h"

#include <math.h>

#include <algorithm>

namespace {

inline uint16_t swap16(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }

// alpha 0..255 of fg over bg, both RGB565 (the blend TFT_eSPI uses).
inline uint16_t mix(uint32_t alpha, uint16_t fg, uint16_t bg) {
    const uint32_t fr = ((fg >> 10) & 0x3E) + 1, fgr = ((fg >> 4) & 0x7E) + 1, fb = ((fg << 1) & 0x3E) + 1;
    const uint32_t br = ((bg >> 10) & 0x3E) + 1, bgr = ((bg >> 4) & 0x7E) + 1, bb = ((bg << 1) & 0x3E) + 1;
    const uint32_t inv = 255 - alpha;
    const uint32_t r = (fr * alpha + br * inv) >> 9;
    const uint32_t g = (fgr * alpha + bgr * inv) >> 9;
    const uint32_t b = (fb * alpha + bb * inv) >> 9;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

} // namespace

void Painter::begin(uint16_t *buffer, int width, int height) {
    buf_ = buffer;
    w_ = width;
    h_ = height;
    cx0_ = cy0_ = 0;
    cx1_ = width;
    cy1_ = height;
}

void Painter::setTransform(float dy, float sx, float sy) {
    dy_ = dy;
    sx_ = sx;
    sy_ = sy;
}

void Painter::resetTransform() {
    setTransform(0, 1, 1);
    rotating_ = false;
}

void Painter::rotate(float px, float py, float angle) {
    rotating_ = angle != 0.0f;
    px_ = px;
    py_ = py;
    cos_ = cosf(angle);
    sin_ = sinf(angle);
}

void Painter::map(float &x, float &y) const {
    if (rotating_) {
        const float X = x - px_, Y = y - py_;
        x = px_ + X * cos_ - Y * sin_;
        y = py_ + X * sin_ + Y * cos_;
    }
    x = kCx + (x - kCx) * sx_;
    y = kFloor + (y - kFloor) * sy_ + dy_;
}

// ---- Pixels -----------------------------------------------------------------

void Painter::span(int y, int x0, int x1, uint16_t c) {
    if (y < cy0_ || y >= cy1_) {
        return;
    }
    x0 = std::max(x0, cx0_);
    x1 = std::min(x1, cx1_);
    const uint16_t v = swap16(c);
    uint16_t *p = buf_ + y * w_;
    for (int x = x0; x < x1; ++x) {
        p[x] = v;
    }
}

void Painter::blend(int x, int y, uint16_t c, float coverage) {
    if (x < cx0_ || x >= cx1_ || y < cy0_ || y >= cy1_ || coverage <= 0.02f) {
        return;
    }
    uint16_t &p = buf_[y * w_ + x];
    if (coverage >= 0.98f) {
        p = swap16(c);
        return;
    }
    p = swap16(mix(static_cast<uint32_t>(coverage * 255.0f), c, swap16(p)));
}

void Painter::rect(int x, int y, int w, int h, uint16_t c) {
    for (int row = y; row < y + h; ++row) {
        span(row, x, x + w, c);
    }
}

// Row by row: pixels inside the ellipse over the whole row are filled
// straight away; the few near the edge get their coverage from the signed
// distance to the ellipse, estimated as F / |grad F|.
void Painter::ellipseAt(float cx, float cy, float rx, float ry, uint16_t c) {
    if (rx < 0.3f || ry < 0.3f) {
        return;
    }
    const float irx2 = 1.0f / (rx * rx), iry2 = 1.0f / (ry * ry);
    const int y0 = std::max(cy0_, static_cast<int>(floorf(cy - ry - 1)));
    const int y1 = std::min(cy1_, static_cast<int>(ceilf(cy + ry + 1)));
    for (int y = y0; y < y1; ++y) {
        const float top = y - cy, bottom = y + 1 - cy;
        const float near = (top <= 0 && bottom >= 0) ? 0.0f : std::min(fabsf(top), fabsf(bottom));
        const float far = std::max(fabsf(top), fabsf(bottom));
        if (near > ry + 1) {
            continue;
        }
        const float outer = near < ry ? rx * sqrtf(1 - near * near * iry2) : 0.0f;
        int fx0 = 0, fx1 = 0;
        if (far < ry) {
            const float inner = rx * sqrtf(1 - far * far * iry2);
            fx0 = static_cast<int>(ceilf(cx - inner));
            fx1 = static_cast<int>(floorf(cx + inner));
            if (fx1 > fx0) {
                span(y, fx0, fx1, c);
            } else {
                fx1 = fx0;
            }
        }
        const float py = y + 0.5f - cy;
        const int ex0 = static_cast<int>(floorf(cx - outer - 1));
        const int ex1 = static_cast<int>(ceilf(cx + outer + 1));
        for (int x = ex0; x < ex1; ++x) {
            if (x >= fx0 && x < fx1) {
                x = fx1 - 1;
                continue;
            }
            const float px = x + 0.5f - cx;
            const float f = px * px * irx2 + py * py * iry2 - 1.0f;
            const float gx = 2 * px * irx2, gy = 2 * py * iry2;
            const float g = sqrtf(gx * gx + gy * gy);
            const float d = g > 1e-6f ? f / g : -1.0f;
            blend(x, y, c, 0.5f - d);
        }
    }
}

// Each edge as a line equation normalised to pixels, positive inside; a
// pixel's coverage comes from its distance to the nearest edge. Per row
// only the columns the triangle can touch are visited.
void Painter::triangleAt(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) {
    const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    float ea[3], eb[3], ec[3];
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3, k = (i + 2) % 3;
        float a = -(ys[j] - ys[i]), b = xs[j] - xs[i];
        const float len = sqrtf(a * a + b * b);
        if (len < 1e-4f) {
            return;
        }
        a /= len;
        b /= len;
        float cc = -(a * xs[i] + b * ys[i]);
        if (a * xs[k] + b * ys[k] + cc < 0) {
            a = -a;
            b = -b;
            cc = -cc;
        }
        ea[i] = a;
        eb[i] = b;
        ec[i] = cc;
    }
    const int yTop = std::max(cy0_, static_cast<int>(floorf(std::min({y0, y1, y2}) - 1)));
    const int yEnd = std::min(cy1_, static_cast<int>(ceilf(std::max({y0, y1, y2}) + 1)));
    const float xMin = std::min({x0, x1, x2}) - 1, xMax = std::max({x0, x1, x2}) + 1;
    for (int y = yTop; y < yEnd; ++y) {
        const float py = y + 0.5f;
        float lo = xMin, hi = xMax;
        bool empty = false;
        for (int i = 0; i < 3; ++i) {
            const float k = eb[i] * py + ec[i];
            // Where this edge's distance is at least -0.5 (partly covered).
            if (ea[i] > 1e-6f) {
                lo = std::max(lo, (-0.5f - k) / ea[i]);
            } else if (ea[i] < -1e-6f) {
                hi = std::min(hi, (-0.5f - k) / ea[i]);
            } else if (k < -0.5f) {
                empty = true;
            }
        }
        if (empty || lo > hi) {
            continue;
        }
        const int xa = static_cast<int>(floorf(lo - 0.5f)), xb = static_cast<int>(ceilf(hi + 0.5f));
        for (int x = xa; x < xb; ++x) {
            const float px = x + 0.5f;
            float m = ea[0] * px + eb[0] * py + ec[0];
            m = std::min(m, ea[1] * px + eb[1] * py + ec[1]);
            m = std::min(m, ea[2] * px + eb[2] * py + ec[2]);
            blend(x, y, c, m + 0.5f);
        }
    }
}

// A capsule: every pixel within r of the segment. Per row only the part of
// the segment that row can reach is considered.
void Painter::lineAt(float x0, float y0, float x1, float y1, float r, uint16_t c) {
    const float reach = r + 1.0f;
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    const int yTop = std::max(cy0_, static_cast<int>(floorf(std::min(y0, y1) - reach)));
    const int yEnd = std::min(cy1_, static_cast<int>(ceilf(std::max(y0, y1) + reach)));
    for (int y = yTop; y < yEnd; ++y) {
        const float py = y + 0.5f;
        float t0 = 0, t1 = 1;
        if (fabsf(dy) > 1e-4f) {
            t0 = (py - reach - y0) / dy;
            t1 = (py + reach - y0) / dy;
            if (t0 > t1) {
                std::swap(t0, t1);
            }
            t0 = std::max(t0, 0.0f);
            t1 = std::min(t1, 1.0f);
            if (t0 > t1) {
                continue;
            }
        }
        const float xa = x0 + dx * t0, xb = x0 + dx * t1;
        const int xs = static_cast<int>(floorf(std::min(xa, xb) - reach));
        const int xe = static_cast<int>(ceilf(std::max(xa, xb) + reach));
        for (int x = xs; x < xe; ++x) {
            const float px = x + 0.5f;
            float t = len2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.0f;
            t = clamp01(t);
            const float qx = px - (x0 + t * dx), qy = py - (y0 + t * dy);
            blend(x, y, c, r + 0.5f - sqrtf(qx * qx + qy * qy));
        }
    }
}

void Painter::roundRectAt(float x, float y, float w, float h, float r, uint16_t c) {
    const float hw = w / 2, hh = h / 2, cx = x + hw, cy = y + hh;
    r = std::min(r, std::min(hw, hh));
    const int yTop = std::max(cy0_, static_cast<int>(floorf(y - 1)));
    const int yEnd = std::min(cy1_, static_cast<int>(ceilf(y + h + 1)));
    const int xs = static_cast<int>(floorf(x - 1)), xe = static_cast<int>(ceilf(x + w + 1));
    for (int row = yTop; row < yEnd; ++row) {
        const float qy = fabsf(row + 0.5f - cy) - (hh - r);
        for (int col = xs; col < xe; ++col) {
            const float qx = fabsf(col + 0.5f - cx) - (hw - r);
            const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            const float d = sqrtf(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
            blend(col, row, c, 0.5f - d);
        }
    }
}

void Painter::arcAt(float x, float y, float r, float half, float a0, float a1, uint16_t c) {
    const float reach = r + half + 1;
    const float ex0 = x + r * cosf(a0), ey0 = y + r * sinf(a0);
    const float ex1 = x + r * cosf(a1), ey1 = y + r * sinf(a1);
    const int yTop = std::max(cy0_, static_cast<int>(floorf(y - reach)));
    const int yEnd = std::min(cy1_, static_cast<int>(ceilf(y + reach)));
    for (int row = yTop; row < yEnd; ++row) {
        const float py = row + 0.5f;
        for (int col = static_cast<int>(floorf(x - reach)); col < static_cast<int>(ceilf(x + reach)); ++col) {
            const float px = col + 0.5f;
            const float a = atan2f(py - y, px - x);
            float d;
            if (a >= a0 && a <= a1) {
                d = fabsf(sqrtf((px - x) * (px - x) + (py - y) * (py - y)) - r);
            } else {
                // Round caps at both ends.
                const float d0 = sqrtf((px - ex0) * (px - ex0) + (py - ey0) * (py - ey0));
                const float d1 = sqrtf((px - ex1) * (px - ex1) + (py - ey1) * (py - ey1));
                d = std::min(d0, d1);
            }
            blend(col, row, c, half + 0.5f - d);
        }
    }
}

void Painter::fade(uint8_t level) {
    if (level == 255) {
        return;
    }
    const int n = w_ * h_;
    for (int i = 0; i < n; ++i) {
        const uint16_t v = swap16(buf_[i]);
        const uint32_t r = ((v >> 11) * level) >> 8;
        const uint32_t g = (((v >> 5) & 0x3F) * level) >> 8;
        const uint32_t b = ((v & 0x1F) * level) >> 8;
        buf_[i] = swap16(static_cast<uint16_t>((r << 11) | (g << 5) | b));
    }
}

// ---- The character's shapes -------------------------------------------------

void Painter::ellipse(float x, float y, float rx, float ry, uint16_t c, bool outlined) {
    if (skip(outlined)) {
        return;
    }
    map(x, y);
    const float g = grown();
    ellipseAt(x, y, std::max(0.3f, rx * sx_ + g), std::max(0.3f, ry * sy_ + g), colour(c));
}

void Painter::triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c,
                       bool outlined) {
    if (skip(outlined)) {
        return;
    }
    map(x0, y0);
    map(x1, y1);
    map(x2, y2);
    const float g = grown();
    if (g > 0) {
        // Grow by pushing the corners away from the middle.
        const float mx = (x0 + x1 + x2) / 3, my = (y0 + y1 + y2) / 3;
        float *xs[3] = {&x0, &x1, &x2}, *ys[3] = {&y0, &y1, &y2};
        for (int i = 0; i < 3; ++i) {
            const float dx = *xs[i] - mx, dy = *ys[i] - my;
            const float d = sqrtf(dx * dx + dy * dy);
            if (d > 1e-4f) {
                *xs[i] += dx / d * g * 1.8f;
                *ys[i] += dy / d * g * 1.8f;
            }
        }
    }
    triangleAt(x0, y0, x1, y1, x2, y2, colour(c));
}

void Painter::line(float x0, float y0, float x1, float y1, float r, uint16_t c, bool outlined) {
    if (skip(outlined)) {
        return;
    }
    map(x0, y0);
    map(x1, y1);
    const float width = std::max(0.5f, r * (sx_ + sy_) / 2 + grown());
    lineAt(x0, y0, x1, y1, width, colour(c));
}

void Painter::rr(float x, float y, float w, float h, float r, uint16_t c) {
    map(x, y);
    const float g = grown();
    roundRectAt(x - g, y - g, w * sx_ + 2 * g, h * sy_ + 2 * g, r + g, colour(c));
}

void Painter::lid(float x, float y, float rx, float ry, float f, uint16_t c) {
    if (f >= 0.95f) {
        // Shut: skin over the eye and a curved line.
        fe(x, y, rx + 0.6f, ry + 0.6f, c);
        fl(x - rx * 0.85f, y + 1, x, y + ry * 0.35f, 0.7f, kInk);
        fl(x, y + ry * 0.35f, x + rx * 0.85f, y + 1, 0.7f, kInk);
        return;
    }
    float X = x, Y = y;
    map(X, Y);
    const float RX = rx * sx_, RY = ry * sy_;
    cx0_ = std::max(0, static_cast<int>(floorf(X - RX - 3)));
    cy0_ = std::max(0, static_cast<int>(floorf(Y - RY - 3)));
    cx1_ = std::min(w_, static_cast<int>(ceilf(X + RX + 3)));
    cy1_ = std::min(h_, static_cast<int>(ceilf(Y - RY - 3 + 3 + 2 * RY * f)));
    fe(x, y, rx + 0.6f, ry + 0.6f, c);
    cx0_ = cy0_ = 0;
    cx1_ = w_;
    cy1_ = h_;
    const float ly = y - ry + 2 * ry * f;
    const float k = (ly - y) / ry;
    const float hw = rx * sqrtf(std::max(0.0f, 1 - k * k));
    fl(x - hw, ly, x + hw, ly, 0.7f, kInk);
}
