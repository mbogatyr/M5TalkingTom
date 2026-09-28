// Runs lib/VoiceChanger on a WAV file on the Mac, to hear the three voices
// without the board.
//
//   c++ -std=gnu++17 -O2 -Ilib/VoiceChanger tools/voice_preview.cpp \
//       lib/VoiceChanger/VoiceChanger.cpp -o /tmp/voice_preview
//   say -v Milena -o /tmp/phrase.aiff "Привет, я говорящий кот"
//   afconvert -f WAVE -d LEI16@16000 -c 1 /tmp/phrase.aiff /tmp/phrase.wav
//   /tmp/voice_preview /tmp/phrase.wav /tmp/out
//
// Writes <out>_cat.wav, <out>_hippo.wav and <out>_mouse.wav. The input must
// be 16-bit mono at 16 kHz, the rate the firmware records at.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "VoiceChanger.h"

static bool readWav(const char *path, std::vector<int16_t> &samples) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    std::vector<uint8_t> data;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        data.insert(data.end(), buf, buf + n);
    }
    fclose(f);
    if (data.size() < 12 || memcmp(data.data(), "RIFF", 4) != 0) {
        return false;
    }
    // Walk the chunks; afconvert adds a few besides fmt and data.
    size_t pos = 12;
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    while (pos + 8 <= data.size()) {
        const uint32_t size = data[pos + 4] | data[pos + 5] << 8 | data[pos + 6] << 16 |
                              static_cast<uint32_t>(data[pos + 7]) << 24;
        if (memcmp(&data[pos], "fmt ", 4) == 0) {
            channels = data[pos + 10] | data[pos + 11] << 8;
            rate = data[pos + 12] | data[pos + 13] << 8 | data[pos + 14] << 16;
            bits = data[pos + 22] | data[pos + 23] << 8;
        } else if (memcmp(&data[pos], "data", 4) == 0) {
            if (channels != 1 || bits != 16 || rate != VoiceChanger::kSampleRate) {
                fprintf(stderr, "need 16-bit mono at 16 kHz, got %u ch, %u bits, %u Hz\n",
                        channels, bits, rate);
                return false;
            }
            const size_t count = std::min<size_t>(size, data.size() - pos - 8) / 2;
            samples.resize(count);
            memcpy(samples.data(), &data[pos + 8], count * 2);
            return true;
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static bool writeWav(const std::string &path, const int16_t *s, size_t n) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + n * 2);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 1);
    put32(f, VoiceChanger::kSampleRate);
    put32(f, VoiceChanger::kSampleRate * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, n * 2);
    fwrite(s, 2, n, f);
    fclose(f);
    return true;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s input.wav output_prefix\n", argv[0]);
        return 2;
    }
    std::vector<int16_t> in;
    if (!readWav(argv[1], in)) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    const char *names[] = {"cat", "hippo", "mouse"};
    for (uint8_t c = 0; c < voices::kCount; ++c) {
        const Voice &v = voices::forCharacter(c);
        std::vector<int16_t> scratch(VoiceChanger::stretchCapacity(in.size(), v));
        std::vector<int16_t> out(VoiceChanger::outputCapacity(in.size(), v));
        VoiceChanger vc;
        vc.start(in.data(), in.size(), scratch.data(), scratch.size(), out.data(), out.size(), v);
        size_t steps = 0;
        while (!vc.step(1)) {
            ++steps;
        }
        const std::string path = std::string(argv[2]) + "_" + names[c] + ".wav";
        writeWav(path, vc.output(), vc.length());
        printf("%-6s %6.2f s -> %6.2f s, %zu steps, %s\n", names[c],
               in.size() / 16000.0, vc.length() / 16000.0, steps, path.c_str());
    }
    return 0;
}
