#include <M5Unified.h>

#include "DisplayTimeout.h"
#include "Renderer.h"

namespace {

constexpr uint8_t kBrightness = 120;

Renderer renderer;
DisplayTimeout displayTimeout;

bool displayAwake = true;

void setDisplayAwake(bool awake) {
    if (awake == displayAwake) {
        return;
    }
    displayAwake = awake;

    if (awake) {
        M5.Display.wakeup();
        M5.Display.setBrightness(kBrightness);
        renderer.invalidate();
    } else {
        // The backlight is the main power consumer, so it is turned off
        // separately from putting the panel itself to sleep.
        M5.Display.setBrightness(0);
        M5.Display.sleep();
    }
}

} // namespace

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Display.setBrightness(kBrightness);
    renderer.begin();
    displayTimeout.begin(millis());
}

void loop() {
    M5.update();

    const uint32_t now = millis();

    // Powering off is not handled here: the side button does it by
    // itself on a double press, via the PMIC.
    const bool activity = M5.BtnA.isPressed() || M5.BtnB.isPressed();
    setDisplayAwake(displayTimeout.shouldBeOn(now, activity));

    if (displayAwake) {
        renderer.draw(now / 1000);
    }

    delay(20);
}
