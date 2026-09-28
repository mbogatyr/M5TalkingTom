#include "VoicePlayer.h"

void VoicePlayer::begin() {
    auto cfg = M5.Speaker.config();
    // M5Unified resamples every sound to this rate; 48 kHz keeps the
    // 16 kHz phrase's interpolation images far above hearing.
    cfg.sample_rate = 48000;
    M5.Speaker.config(cfg);
}

void VoicePlayer::play(const int16_t *samples, size_t len, uint32_t sampleRate) {
    if (!on_) {
        on_ = M5.Speaker.begin();
        M5.Speaker.setVolume(kVolume);
    }
    M5.Speaker.playRaw(samples, len, sampleRate, false, 1, 0, true);
}

void VoicePlayer::stop() {
    if (!on_) {
        return;
    }
    M5.Speaker.stop();
    M5.Speaker.end();
    on_ = false;
}

bool VoicePlayer::playing() const { return on_ && M5.Speaker.isPlaying(); }
