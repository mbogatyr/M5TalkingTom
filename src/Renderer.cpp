#include "Renderer.h"

void Renderer::begin() {
    M5.Display.setRotation(0); // portrait: 135 wide, 240 tall, KEY1 below
    M5.Display.fillScreen(TFT_BLACK);

    canvas_.setColorDepth(16);
    canvas_.setPsram(true); // 135*240*2 = 65 KB; the StickS3 has 8 MB of PSRAM
    canvas_.createSprite(M5.Display.width(), M5.Display.height());
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

// Placeholder until the characters are ported from the prototype.
void Renderer::paint(const Frame &f) {
    static const char *const kNames[] = {"Cat", "Hippo", "Mouse"};
    static const char *const kModes[] = {"listening", "hearing", "thinking", "talking", "goodbye"};
    static const uint16_t kColours[] = {0x8D17, 0x9C79, 0xACD1};

    canvas_.fillSprite(kColours[f.character % 3]);
    canvas_.setTextDatum(middle_center);
    canvas_.setTextColor(TFT_WHITE);
    canvas_.setFont(&fonts::Font4);
    canvas_.drawString(kNames[f.character % 3], 67, 40);
    canvas_.setFont(&fonts::Font2);
    canvas_.drawString(kModes[static_cast<int>(f.mode)], 67, 70);
    if (f.sleepy) {
        canvas_.drawString("zzz", 67, 90);
    }
    const int mouth = 2 + f.mouth * 40 / 255;
    canvas_.fillEllipse(67, 150, 30, mouth, TFT_BLACK);
    canvas_.fillRect(10, 220, f.micLevel * 115 / 255, 8, TFT_WHITE);
}
