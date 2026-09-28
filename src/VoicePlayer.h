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
    // Measured with the Mac's microphone on 2026-09-28: M5Unified's StickS3
    // settings (magnification 1) play a -1 dBFS phrase about 20 dB below
    // what the speaker can do. Each doubling of the magnification added
    // 6 dB up to 4; at 8 the peaks clip (only +4.8 dB).
    static constexpr uint8_t kVolume = 255;
    static constexpr uint8_t kMagnification = 4;

    // Call once after M5.begin().
    void begin();

    void play(const int16_t *samples, size_t len, uint32_t sampleRate);
    // Stops early (KEY1) or releases the speaker after a playback ended.
    void stop();

    bool playing() const;

    void setVolume(uint8_t volume) { volume_ = volume; }
    uint8_t volume() const { return volume_; }

  private:
    bool on_ = false;
    uint8_t volume_ = kVolume;
};
