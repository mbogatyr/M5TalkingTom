#pragma GCC optimize("O2")

#include "Renderer.h"

#include <math.h>

namespace {

constexpr int W = 135, H = 240;
constexpr float kPi = 3.14159265f;
constexpr uint16_t kInk = Painter::kInk;
constexpr uint16_t kWhite = rgb(0xFFFFFF);
constexpr uint16_t kPupil = rgb(0x15121A);

namespace cat {
constexpr uint16_t fur = rgb(0x8FA3BF), dark = rgb(0x6A7F9E), light = rgb(0xEEF1F6);
constexpr uint16_t pink = rgb(0xF2A7B8), nose = rgb(0xE26D8A), iris = rgb(0x7CC45A);
constexpr uint16_t bg = rgb(0xF6E3C3), bg2 = rgb(0xE9CFA6), floor = rgb(0xB98352), floor2 = rgb(0xA5703F);
constexpr uint16_t mouth = rgb(0x6A1F2F), tongue = rgb(0xF07C93);
} // namespace cat

namespace hippo {
constexpr uint16_t skin = rgb(0x9D8CC8), belly = rgb(0xC9BDE8), muzzle = rgb(0xB8A8DE);
constexpr uint16_t pink = rgb(0xF2A7B8), sky = rgb(0xCDEBF5), water = rgb(0x4FA3C7), water2 = rgb(0x8FCBE3);
constexpr uint16_t mouth = rgb(0x7A2340), tongue = rgb(0xEE7C98), nostril = rgb(0x5B4A86), blush = rgb(0xE6A0C8);
} // namespace hippo

namespace mouse {
constexpr uint16_t fur = rgb(0xA99B8D), light = rgb(0xEFE5DA), pink = rgb(0xF4A9BB), nose = rgb(0xE8738F);
constexpr uint16_t bg = rgb(0xDDEFD8), bg2 = rgb(0xCBE4C4), floor = rgb(0xC7A57B);
constexpr uint16_t cheese = rgb(0xF6C945), hole = rgb(0xDDA42A), mouth = rgb(0x5A1830), tooth = rgb(0xB9B0A8);
} // namespace mouse

const char *const kNames[] = {"Cat", "Hippo", "Mouse"};

// A cheap integer hash to [0, 1): which blinks and ear twitches happen.
float hash01(uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    n ^= n >> 16;
    return static_cast<float>(n & 0xFFFFFF) / 16777216.0f;
}

float fmodPos(float a, float b) {
    const float r = fmodf(a, b);
    return r < 0 ? r + b : r;
}

// The switch animation at `e` seconds: which character shows (true = the
// new one) and how it is moved and squashed.
struct Switch {
    bool incoming;
    float dy, sx, sy;
};
Switch switchAt(float e) {
    if (e < 0.25f) {
        const float v = e / 0.25f;
        return {false, 260 * v * v, 1, 1};
    }
    const float u = (e - 0.25f) / 0.6f;
    if (u < 0.55f) {
        const float v = u / 0.55f;
        return {true, -250 * (1 - v * v), 0.9f, 1.1f};
    }
    if (u < 0.75f) {
        const float s = sinf((u - 0.55f) / 0.2f * kPi);
        return {true, 0, 1 + 0.16f * s, 1 - 0.14f * s};
    }
    if (u < 1) {
        return {true, -7 * sinf((u - 0.75f) / 0.25f * kPi), 1, 1};
    }
    return {true, 0, 1, 1};
}

} // namespace

void Renderer::begin() {
    M5.Display.setRotation(0); // portrait: 135 wide, 240 tall
    M5.Display.fillScreen(TFT_BLACK);

    canvas_.setColorDepth(16);
    // 135*240*2 = 65 KB in internal RAM: Painter reads and writes single
    // pixels, and through the PSRAM cache that made a frame take 26 ms.
    canvas_.setPsram(false);
    if (canvas_.createSprite(M5.Display.width(), M5.Display.height()) == nullptr) {
        canvas_.setPsram(true);
        canvas_.createSprite(M5.Display.width(), M5.Display.height());
    }
    p_.begin(static_cast<uint16_t *>(canvas_.getBuffer()), canvas_.width(), canvas_.height());
}

void Renderer::draw(const Frame &f) {
    const uint32_t t0 = micros();
    paint(f);
    const uint32_t t1 = micros();
    canvas_.pushSprite(0, 0);
    pushUs_ = micros() - t1;
    paintUs_ = t1 - t0;
}

void Renderer::writeSnapshot(Print &out) {
    out.printf("SNAP %d %d\n", canvas_.width(), canvas_.height());
    // A 16-bit LovyanGFX sprite already keeps its pixels byte-swapped for
    // SPI, i.e. high byte first, so the buffer goes out as it is.
    out.write(static_cast<const uint8_t *>(canvas_.getBuffer()),
              canvas_.width() * canvas_.height() * 2);
    out.flush();
}

void Renderer::paint(const Frame &f) {
    // Each character's clock is offset so that they never blink in step.
    uint8_t ch = f.character;
    Switch sw{true, 0, 1, 1};
    Look look;
    switch (f.mode) {
    case Mode::Hearing:
        look = Look::Hear;
        break;
    case Mode::Thinking:
        look = Look::Think;
        break;
    case Mode::Talking:
        look = Look::Talk;
        break;
    case Mode::Goodbye:
        look = Look::Bye;
        break;
    case Mode::Listening:
    default:
        look = f.sleepy ? Look::Sleepy : Look::Idle;
        break;
    }
    if (f.switching) {
        sw = switchAt(f.switchMs / 1000.0f);
        if (!sw.incoming) {
            ch = f.previousCharacter;
        }
        look = Look::Idle;
    }
    const float t = f.timeMs / 1000.0f + ch * 1.7f;

    background(ch, t);
    const Pose p = pose(ch, look, t, f);
    p_.setTransform(sw.dy, sw.sx, sw.sy);
    Anchors a;
    switch (ch) {
    case 1:
        a = drawHippo(p, t);
        break;
    case 2:
        a = drawMouse(p, t);
        break;
    default:
        a = drawCat(p, t);
        break;
    }
    p_.resetTransform();
    foreground(ch, t);
    if (!f.switching) {
        overlays(look, a, p, t);
    }
    topBar(ch);

    if (look == Look::Bye && f.modeMs > kWaveMs) {
        const uint32_t into = f.modeMs - kWaveMs;
        const uint32_t level = into >= kFadeMs ? 0 : 255 - into * 255 / kFadeMs;
        p_.fade(static_cast<uint8_t>(level));
    }
}

Renderer::Pose Renderer::pose(uint8_t ch, Look look, float t, const Frame &f) const {
    // The hippo is slower, the mouse quicker.
    const float sp = ch == 1 ? 0.7f : (ch == 2 ? 1.4f : 1.0f);
    Pose p;
    p.look = look;
    p.breath = sinf(t * 2 * kPi * 0.45f * sp);

    const float per = 3.3f;
    const uint32_t k = static_cast<uint32_t>(t / per);
    const float ph = t - k * per, at = hash01(k) * 2.4f + 0.3f;
    if (ph > at && ph < at + 0.14f) {
        p.lid = 1;
    }
    const uint32_t k2 = static_cast<uint32_t>(t / 2.7f);
    const float ph2 = t - k2 * 2.7f;
    if (hash01(k2 + 50) > 0.45f && ph2 < 0.3f) {
        const float v = sinf(ph2 / 0.3f * kPi) * 4;
        if (hash01(k2 + 7) > 0.5f) {
            p.earL = v;
        } else {
            p.earR = v;
        }
    }

    switch (look) {
    case Look::Talk: {
        p.mouth = f.mouth / 255.0f;
        const float g = 0.35f + 0.65f * p.mouth;
        p.gL = fmaxf(0, sinf(t * 2.2f * sp)) * g;
        p.gR = fmaxf(0, sinf(t * 2.2f * sp + kPi)) * g;
        p.bob = -p.mouth * 3;
        if (ch == 2) {
            p.hop = -p.mouth * 5;
        }
        break;
    }
    case Look::Hear:
        p.tilt = 0.17f;
        p.level = f.micLevel / 255.0f;
        p.earR = -3;
        p.lookX = 2;
        break;
    case Look::Think:
        p.lookX = 2;
        p.lookY = -3;
        p.tilt = -0.07f;
        break;
    case Look::Sleepy: {
        p.lid = fmaxf(p.lid, 0.62f + 0.28f * sinf(t * 0.9f));
        const float y = fmodPos(t, 5.5f);
        if (y < 1.6f) {
            p.mouth = sinf(y / 1.6f * kPi) * 0.85f;
            p.lid = fmaxf(p.lid, 0.85f);
        }
        p.tilt = 0.07f * sinf(t * 0.5f);
        break;
    }
    case Look::Bye:
        p.wave = sinf(t * 10);
        p.mouth = 0.35f + 0.25f * sinf(t * 6);
        break;
    case Look::Idle:
        if (ch == 2) {
            const float h = fmodPos(t, 3.6f);
            if (h < 0.42f) {
                p.hop = -sinf(h / 0.42f * kPi) * 9;
            }
        }
        break;
    }
    return p;
}

void Renderer::eye(float x, float y, float rx, float ry, uint16_t iris, float ir, float prx,
                   float pry, uint16_t lidColour, float lid, const Pose &p) {
    Painter &P = p_;
    P.part([&] { P.e(x, y, rx, ry, kWhite); }, 1.5f);
    if (iris != 0) {
        P.fe(x + p.lookX, y + 1 + p.lookY, ir, ir * 1.1f, iris);
    }
    P.fe(x + p.lookX, y + 1 + p.lookY, prx, pry, kPupil);
    P.fc(x + p.lookX + prx * 0.5f + 1, y - 2 + p.lookY, 1.6f, kWhite);
    if (lid > 0.02f) {
        P.lid(x, y, rx, ry, fminf(1, lid), lidColour);
    }
}

// ---- The cat ------------------------------------------------------------------

Renderer::Anchors Renderer::drawCat(const Pose &p, float t) {
    Painter &P = p_;
    const float b = p.breath, bodyY = 170 - b * 1.2f, hy = 104 - b * 1.8f + p.bob, m = p.mouth;

    // The tail, behind everything.
    const float sw = sinf(t * 1.7f) * (p.look == Look::Talk ? 1.4f : 1.0f);
    float qx[13], qy[13];
    for (int i = 0; i <= 12; ++i) {
        const float u = i / 12.0f;
        qx[i] = 88 + 26 * sinf(u * 1.4f) + sw * 10 * u * u;
        qy[i] = 206 - 70 * u;
    }
    P.part([&] {
        for (int i = 0; i < 12; ++i) {
            P.l(qx[i], qy[i], qx[i + 1], qy[i + 1], 4.2f, i >= 9 ? cat::dark : cat::fur);
        }
    });
    P.part([&] { P.e(67, bodyY, 28, 38 + b * 0.8f, cat::fur); });
    P.fe(67, bodyY + 9, 17, 25, cat::light);
    P.part([&] {
        P.e(51, 214, 13, 7, cat::fur);
        P.e(83, 214, 13, 7, cat::fur);
    });

    P.rotate(67, hy + 34, p.tilt);
    P.part([&] {
        P.t(34, hy - 8, 40, hy - 52 + p.earL, 62, hy - 26, cat::fur);
        P.t(100, hy - 8, 94, hy - 52 + p.earR, 72, hy - 26, cat::fur);
    });
    P.ft(40, hy - 14, 43, hy - 42 + p.earL, 56, hy - 26, cat::pink);
    P.ft(94, hy - 14, 91, hy - 42 + p.earR, 78, hy - 26, cat::pink);
    P.part([&] {
        P.e(67, hy, 37, 31, cat::fur);
        P.t(26, hy + 10, 40, hy, 40, hy + 20, cat::fur);
        P.t(108, hy + 10, 94, hy, 94, hy + 20, cat::fur);
    });
    P.fl(60, hy - 30, 60, hy - 23, 1.1f, cat::dark);
    P.fl(67, hy - 31, 67, hy - 23, 1.1f, cat::dark);
    P.fl(74, hy - 30, 74, hy - 23, 1.1f, cat::dark);
    P.fe(67, hy + 15 + m * 2, 22, 13 + m * 3, cat::light);
    eye(53, hy - 5, 10, 12, cat::iris, 7.5f, 2.8f, 6, cat::fur, p.lid, p);
    eye(81, hy - 5, 10, 12, cat::iris, 7.5f, 2.8f, 6, cat::fur, p.lid, p);
    if (m > 0.06f) {
        P.part([&] { P.e(67, hy + 17 + m * 5, 5 + m * 6, 1.5f + m * 9, cat::mouth); }, 1.2f);
        P.fe(67, hy + 19 + m * 10, 3 + m * 4, 1 + m * 3.5f, cat::tongue);
    } else {
        // A cat's closed mouth: a little "w" under the nose.
        P.fl(67, hy + 10, 67, hy + 13, 0.6f, kInk);
        P.fl(67, hy + 13, 63, hy + 16, 0.6f, kInk);
        P.fl(63, hy + 16, 59, hy + 13, 0.6f, kInk);
        P.fl(67, hy + 13, 71, hy + 16, 0.6f, kInk);
        P.fl(71, hy + 16, 75, hy + 13, 0.6f, kInk);
    }
    P.fe(67, hy + 7, 5, 3.5f, cat::nose);
    for (int s = -1; s <= 1; s += 2) {
        P.fl(67 + s * 16, hy + 13, 67 + s * 40, hy + 9, 0.5f, kInk);
        P.fl(67 + s * 16, hy + 16, 67 + s * 40, hy + 18, 0.5f, kInk);
    }
    P.rotate(0, 0, 0);

    float plx = 41, ply = bodyY + 14 + sinf(t * 1.3f), prx = 93, pry = bodyY + 14 - sinf(t * 1.3f);
    if (p.look == Look::Talk) {
        plx = 38 - 8 * p.gL;
        ply = bodyY + 10 - 30 * p.gL;
        prx = 96 + 8 * p.gR;
        pry = bodyY + 10 - 30 * p.gR;
    } else if (p.look == Look::Hear) {
        prx = 110;
        pry = hy - 2;
    } else if (p.look == Look::Think) {
        prx = 80;
        pry = hy + 36;
    } else if (p.look == Look::Bye) {
        prx = 106 + p.wave * 7;
        pry = hy - 28;
    }
    P.part([&] {
        P.l(46, bodyY - 24, plx, ply, 6, cat::fur);
        P.c(plx, ply, 7, cat::fur);
    });
    P.part([&] {
        P.l(88, bodyY - 24, prx, pry, 6, cat::fur);
        P.c(prx, pry, 7, cat::fur);
    });
    return {116, hy - 14, 85, hy - 40};
}

// ---- The hippo ----------------------------------------------------------------

Renderer::Anchors Renderer::drawHippo(const Pose &p, float t) {
    Painter &P = p_;
    const float b = p.breath, bodyY = 182 - b * 1.6f, hy = 96 - b * 2.2f + p.bob, m = p.mouth;
    P.part([&] { P.e(67, bodyY, 50, 44 + b, hippo::skin); });
    P.fe(67, bodyY + 12, 33, 30, hippo::belly);

    P.rotate(67, hy + 40, p.tilt);
    P.part([&] {
        P.e(42, hy - 38 + p.earL, 7, 8, hippo::skin);
        P.e(92, hy - 38 + p.earR, 7, 8, hippo::skin);
    });
    P.fe(42, hy - 37 + p.earL, 3.5f, 4.5f, hippo::pink);
    P.fe(92, hy - 37 + p.earR, 3.5f, 4.5f, hippo::pink);
    P.part([&] {
        P.e(67, hy - 12, 31, 26, hippo::skin);
        P.c(52, hy - 28, 12, hippo::skin);
        P.c(82, hy - 28, 12, hippo::skin);
    });
    // Sleepy-looking heavy lids, always a little down.
    const float lid = fmaxf(p.lid, 0.28f);
    eye(52, hy - 28, 7.5f, 7.5f, 0, 0, 3.4f, 3.8f, hippo::skin, lid, p);
    eye(82, hy - 28, 7.5f, 7.5f, 0, 0, 3.4f, 3.8f, hippo::skin, lid, p);

    // The lower jaw drops and the upper muzzle lifts a little.
    const float lowY = hy + 26 + m * 36, upY = hy + 12 - m * 6;
    if (m > 0.06f) {
        P.part([&] { P.e(67, (upY + lowY) / 2 + 2, 32, (lowY - upY) / 2 + 4, hippo::mouth); }, 1.5f);
        P.fe(67, lowY - 14, 21, 3 + m * 5, hippo::tongue);
    }
    P.part([&] { P.e(67, lowY, 36, 14, hippo::muzzle); });
    if (m > 0.06f) {
        const float th = 7 + m * 7;
        P.part([&] {
            P.rr(41, lowY - 9 - th, 7, th + 3, 3, kWhite);
            P.rr(86, lowY - 9 - th, 7, th + 3, 3, kWhite);
        }, 1.2f);
    }
    P.part([&] { P.e(67, upY, 42, 22, hippo::muzzle); });
    P.fe(53, upY - 8, 4.5f, 3.2f, hippo::nostril);
    P.fe(81, upY - 8, 4.5f, 3.2f, hippo::nostril);
    P.fe(33, upY + 5, 6, 3.5f, hippo::blush);
    P.fe(101, upY + 5, 6, 3.5f, hippo::blush);
    if (m <= 0.06f) {
        P.fl(40, upY + 13, 52, upY + 17, 0.8f, kInk);
        P.fl(52, upY + 17, 82, upY + 17, 0.8f, kInk);
        P.fl(82, upY + 17, 94, upY + 13, 0.8f, kInk);
    }
    P.rotate(0, 0, 0);

    float plx = 20, ply = bodyY + 8 + sinf(t * 0.9f), prx = 114, pry = bodyY + 8 - sinf(t * 0.9f);
    if (p.look == Look::Talk) {
        plx = 16 - 4 * p.gL;
        ply = bodyY + 4 - 30 * p.gL;
        prx = 118 + 4 * p.gR;
        pry = bodyY + 4 - 30 * p.gR;
    } else if (p.look == Look::Hear) {
        prx = 118;
        pry = hy + 18;
    } else if (p.look == Look::Think) {
        prx = 96;
        pry = hy + 46;
    } else if (p.look == Look::Bye) {
        prx = 120 + p.wave * 5;
        pry = hy + 2;
    }
    P.part([&] {
        P.l(28, bodyY - 18, plx, ply, 8, hippo::skin);
        P.c(plx, ply, 9.5f, hippo::skin);
    });
    P.part([&] {
        P.l(106, bodyY - 18, prx, pry, 8, hippo::skin);
        P.c(prx, pry, 9.5f, hippo::skin);
    });
    return {106, hy - 40, 84, hy - 44};
}

// ---- The mouse ----------------------------------------------------------------

Renderer::Anchors Renderer::drawMouse(const Pose &p, float t) {
    Painter &P = p_;
    const float b = p.breath, h = p.hop, bodyY = 182 - b + h, hy = 130 - b * 1.4f + h + p.bob, m = p.mouth;

    float qx[17], qy[17];
    for (int i = 0; i <= 16; ++i) {
        const float u = i / 16.0f;
        qx[i] = 84 + 36 * u + 5 * sinf(u * 8 + t * 3) * u;
        qy[i] = 206 - 10 * sinf(u * kPi) - 44 * u * u + h * (1 - u);
    }
    P.part([&] {
        for (int i = 0; i < 16; ++i) {
            P.l(qx[i], qy[i], qx[i + 1], qy[i + 1], 1.8f, mouse::pink);
        }
    }, 1.2f);
    P.part([&] { P.e(67, bodyY, 23, 30 + b * 0.6f, mouse::fur); });
    P.fe(67, bodyY + 7, 15, 20, mouse::light);
    P.part([&] {
        P.e(55, 214 + h, 11, 4.5f, mouse::pink);
        P.e(79, 214 + h, 11, 4.5f, mouse::pink);
    }, 1.5f);

    P.rotate(67, hy + 24, p.tilt);
    P.part([&] {
        P.c(40, hy - 26 + p.earL, 21, mouse::fur);
        P.c(94, hy - 26 + p.earR, 21, mouse::fur);
    });
    P.fc(40, hy - 25 + p.earL, 14, mouse::pink);
    P.fc(94, hy - 25 + p.earR, 14, mouse::pink);
    P.part([&] {
        P.e(67, hy, 27, 23, mouse::fur);
        P.e(67, hy + 11, 15, 11, mouse::fur);
    });
    P.fe(67, hy + 13, 11, 7, mouse::light);
    for (int i = 0; i < 2; ++i) {
        const float x = i == 0 ? 56 : 78;
        P.fe(x + p.lookX, hy - 4 + p.lookY, 6, 8, kPupil);
        P.fc(x + 2 + p.lookX, hy - 7 + p.lookY, 2.2f, kWhite);
        P.fc(x - 2 + p.lookX, hy - 1 + p.lookY, 1, kWhite);
        if (p.lid > 0.02f) {
            P.lid(x, hy - 4, 6.5f, 8.5f, fminf(1, p.lid), mouse::fur);
        }
    }
    // Sniffing now and then.
    const float sn = p.look == Look::Idle && sinf(t * 0.9f) > 0.5f ? sinf(t * 16) : 0;
    if (m > 0.06f) {
        P.fe(67, hy + 24 + m * 3, 3.5f + m * 4, 1 + m * 6, mouse::mouth);
    } else {
        P.fl(63, hy + 23, 67, hy + 25, 0.5f, kInk);
        P.fl(67, hy + 25, 71, hy + 23, 0.5f, kInk);
    }
    P.part([&] { P.rr(64, hy + 19, 6, 5.5f, 1.2f, kWhite); }, 1);
    P.fl(67, hy + 20, 67, hy + 24, 0.4f, mouse::tooth);
    P.fc(67, hy + 15 + sn, 4.5f, mouse::nose);
    P.fc(68.5f, hy + 13.5f + sn, 1.3f, kWhite);
    const float tw = p.look == Look::Idle ? sinf(t * 9) * (sinf(t * 0.7f) > 0.3f ? 1.5f : 0) : 0;
    for (int s = -1; s <= 1; s += 2) {
        P.fl(67 + s * 10, hy + 15, 67 + s * 42, hy + 8 + tw, 0.5f, kInk);
        P.fl(67 + s * 10, hy + 18, 67 + s * 42, hy + 19 - tw, 0.5f, kInk);
    }
    P.rotate(0, 0, 0);

    float plx = 61, ply = bodyY - 4, prx = 73, pry = bodyY - 4;
    if (p.look == Look::Talk) {
        plx = 44 - 6 * p.gL;
        ply = bodyY - 10 - 22 * p.gL;
        prx = 90 + 6 * p.gR;
        pry = bodyY - 10 - 22 * p.gR;
    } else if (p.look == Look::Hear) {
        prx = 105;
        pry = hy - 4;
    } else if (p.look == Look::Think) {
        prx = 76;
        pry = hy + 30;
    } else if (p.look == Look::Bye) {
        prx = 109 + p.wave * 6;
        pry = hy - 16;
    }
    P.part([&] {
        P.l(51, bodyY - 18, plx, ply, 2.8f, mouse::fur);
        P.c(plx, ply, 4.2f, mouse::pink);
    }, 1.5f);
    P.part([&] {
        P.l(83, bodyY - 18, prx, pry, 2.8f, mouse::fur);
        P.c(prx, pry, 4.2f, mouse::pink);
    }, 1.5f);
    return {121, hy - 6, 100, hy - 46};
}

// ---- Scenery ------------------------------------------------------------------

void Renderer::background(uint8_t ch, float t) {
    Painter &P = p_;
    if (ch == 0) {
        P.rect(0, 0, W, H, cat::bg);
        P.rect(0, 170, W, 36, cat::bg2);
        P.rect(0, 168, W, 2, cat::floor2);
        P.rect(0, 206, W, 34, cat::floor);
        for (int x : {18, 52, 86, 120}) {
            P.rect(x, 206, 1, 34, cat::floor2);
        }
        P.rect(0, 206, W, 1, cat::floor2);
        P.ellipseAt(67, 221, 52, 9, rgb(0xC9594D));
        P.ellipseAt(67, 221, 40, 6, rgb(0xE07A6C));
        // A picture of a fish on the wall.
        P.rect(10, 32, 28, 22, kInk);
        P.rect(12, 34, 24, 18, rgb(0x9FD3E8));
        P.ellipseAt(23, 43, 6, 3.5f, rgb(0xF29A4A));
        P.triangleAt(28, 43, 33, 39, 33, 47, rgb(0xF29A4A));
    } else if (ch == 1) {
        P.rect(0, 0, W, H, hippo::sky);
        P.ellipseAt(20, 38, 11, 11, rgb(0xFFD45C));
        P.rect(0, 188, W, 16, rgb(0x9FD28A));
        static const float reeds[][3] = {{8, 5, 158}, {11, 14, 164}, {15, 20, 170}, {124, 121, 166}, {128, 131, 160}};
        for (const auto &r : reeds) {
            P.lineAt(r[0], 200, r[1] + sinf(t * 1.2f + r[0]) * 1.5f, r[2], 1, rgb(0x5E9E4B));
        }
    } else {
        P.rect(0, 0, W, H, mouse::bg);
        P.rect(0, 196, W, 10, mouse::bg2);
        P.rect(0, 206, W, 34, mouse::floor);
        // A mouse hole and a piece of cheese.
        P.ellipseAt(24, 186, 15, 12, kInk);
        P.rect(9, 186, 30, 20, kInk);
        P.ellipseAt(24, 186, 13, 10, rgb(0x3B2F2B));
        P.rect(11, 186, 26, 20, rgb(0x3B2F2B));
        P.triangleAt(95, 216, 132, 216, 132, 192, kInk);
        P.triangleAt(99, 214.5f, 130.5f, 214.5f, 130.5f, 195, mouse::cheese);
        P.ellipseAt(122, 208, 3, 2.5f, mouse::hole);
        P.ellipseAt(127, 201, 2, 2, mouse::hole);
        P.ellipseAt(113, 211, 2, 1.6f, mouse::hole);
    }
}

// In front of the character: the hippo's pond.
void Renderer::foreground(uint8_t ch, float t) {
    if (ch != 1) {
        return;
    }
    Painter &P = p_;
    P.rect(0, 204, W, 36, hippo::water);
    const float drift = fmodPos(t * 8, 12);
    for (int x = -12; x < W + 12; x += 12) {
        P.ellipseAt(x + drift, 204, 7, 3.5f, hippo::water);
    }
    static const float ripples[][3] = {{20, 214, 12}, {70, 222, 16}, {104, 212, 10}, {40, 230, 14}, {92, 232, 12}};
    for (const auto &r : ripples) {
        const float o = sinf(t * 1.5f + r[0]) * 3;
        P.lineAt(r[0] + o, r[1], r[0] + o + r[2], r[1], 0.75f, hippo::water2);
    }
}

// Sound waves at the ear, floating "z", the goodbye bubble.
void Renderer::overlays(Look look, const Anchors &a, const Pose &p, float t) {
    Painter &P = p_;
    if (look == Look::Hear) {
        const int n = 1 + static_cast<int>(p.level * 2.99f);
        for (int i = 0; i < n; ++i) {
            P.arcAt(a.earX - 6, a.earY, 6 + i * 5, 1, -0.8f, 0.8f, kInk);
        }
    } else if (look == Look::Sleepy) {
        for (int i = 0; i < 3; ++i) {
            const float ph = fmodPos(t * 0.45f + i / 3.0f, 1);
            const float size = (8 + ph * 7) * 0.55f;
            const float x = a.headX + 8 + i * 6 + sinf(ph * 6) * 2;
            const float y = fmaxf(28, a.headY - ph * 24);
            const float hs = size / 2;
            P.lineAt(x - hs, y - hs, x + hs, y - hs, 1.0f, kInk);
            P.lineAt(x + hs, y - hs, x - hs, y + hs, 1.0f, kInk);
            P.lineAt(x - hs, y + hs, x + hs, y + hs, 1.0f, kInk);
        }
    } else if (look == Look::Bye) {
        P.roundRectAt(6, 24, 50, 24, 7, kInk);
        P.triangleAt(28, 46, 44, 46, 47, 58, kInk);
        P.roundRectAt(7.5f, 25.5f, 47, 21, 6, kWhite);
        P.triangleAt(30, 45, 42.5f, 45, 45, 54.5f, kWhite);
        canvas_.setFont(&fonts::FreeSansBold9pt7b);
        canvas_.setTextDatum(middle_center);
        canvas_.setTextColor(kInk);
        canvas_.drawString("Bye!", 31, 36);
    }
}

void Renderer::topBar(uint8_t ch) {
    p_.rect(0, 0, W, 18, rgb(0x1B1720));
    canvas_.setFont(&fonts::FreeSansBold9pt7b);
    canvas_.setTextDatum(middle_left);
    canvas_.setTextColor(kWhite);
    canvas_.drawString(kNames[ch % 3], 6, 9);
    for (int i = 0; i < 3; ++i) {
        const float r = i == ch ? 3.2f : 2.6f;
        p_.ellipseAt(107 + i * 10, 9.5f, r, r, i == ch ? kWhite : rgb(0x5A5462));
    }
}
