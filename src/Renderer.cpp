#include "Renderer.h"

void Renderer::begin() {
    M5.Display.setRotation(0); // portrait: 135 wide, 240 tall
    M5.Display.fillScreen(TFT_BLACK);

    canvas_.setColorDepth(16);
    canvas_.setPsram(true); // 135*240*2 = 65 KB; the StickS3 has 8 MB of PSRAM
    canvas_.createSprite(M5.Display.width(), M5.Display.height());
}

void Renderer::invalidate() { hasPrevious_ = false; }

void Renderer::draw(uint32_t uptimeSeconds) {
    const bool unchanged = hasPrevious_ && previousSeconds_ == uptimeSeconds;
    if (unchanged) {
        return;
    }

    paint(uptimeSeconds);

    hasPrevious_ = true;
    previousSeconds_ = uptimeSeconds;
}

void Renderer::paint(uint32_t uptimeSeconds) {
    const int centerX = canvas_.width() / 2;
    const int centerY = canvas_.height() / 2;

    canvas_.fillSprite(TFT_BLACK);
    canvas_.setTextDatum(middle_center);
    canvas_.setTextColor(TFT_WHITE);

    canvas_.setFont(&fonts::Font2);
    canvas_.drawString("M5StickS3", centerX, centerY - 30);

    canvas_.setFont(&fonts::Font4);
    canvas_.drawString(String(uptimeSeconds) + " s", centerX, centerY + 10);

    canvas_.pushSprite(0, 0);
}
