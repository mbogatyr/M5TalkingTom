// A stand-in for M5Unified, just enough to build src/Renderer.cpp and
// src/Painter.cpp on the Mac. Text is not drawn: the name in the top bar
// and "Bye!" are missing from the previews.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <vector>

inline uint32_t micros() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

class Print {
  public:
    virtual ~Print() = default;
    virtual size_t write(const uint8_t *, size_t n) { return n; }
    int printf(const char *, ...) { return 0; }
    void flush() {}
};

namespace fonts {
extern const int FreeSansBold9pt7b;
}
enum { middle_center, middle_left };
constexpr uint16_t TFT_BLACK = 0;

class M5Canvas {
  public:
    explicit M5Canvas(void *) {}
    void setColorDepth(int) {}
    void setPsram(bool) {}
    void *createSprite(int w, int h) {
        w_ = w;
        h_ = h;
        pixels_.assign(static_cast<size_t>(w * h), 0);
        return pixels_.data();
    }
    void *getBuffer() { return pixels_.data(); }
    int width() const { return w_; }
    int height() const { return h_; }
    void pushSprite(int, int) {}
    void setFont(const void *) {}
    void setTextDatum(int) {}
    void setTextColor(uint16_t) {}
    void drawString(const char *, int, int) {}

  private:
    int w_ = 0, h_ = 0;
    std::vector<uint16_t> pixels_;
};

struct M5Stub {
    struct {
        void setRotation(int) {}
        void fillScreen(uint16_t) {}
        int width() const { return 135; }
        int height() const { return 240; }
    } Display;
};
extern M5Stub M5;
