#pragma once

#include <M5Unified.h>

// Plays a changed phrase on the StickS3's speaker (ES8311 DAC and the amp
// behind the PMIC).
//
// The speaker is only switched on for the playback: it shares the I2S
// clock lines with the mic. The samples are not copied, so the buffer must
// stay untouched until playing() turns false.
class VoicePlayer {
  public:
    static constexpr uint8_t kVolume = 200;

    // Call once after M5.begin().
    void begin();

    void play(const int16_t *samples, size_t len, uint32_t sampleRate);
    // Stops early (KEY1) or releases the speaker after a playback ended.
    void stop();

    bool playing() const;

  private:
    bool on_ = false;
};
