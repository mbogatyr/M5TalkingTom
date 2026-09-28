// Draws the characters with the firmware's own Renderer on the Mac and
// saves a contact sheet, to check the look without the board.
//
//   c++ -std=gnu++17 -O2 -Itools/render_preview -Isrc -Ilib/TalkState \
//       tools/render_preview/main.cpp src/Renderer.cpp src/Painter.cpp \
//       -lz -o /tmp/render_preview
//   /tmp/render_preview /tmp/sheet.png [time_ms]
//
// Rows: cat, hippo, mouse. Columns: listening, hearing, thinking, talking,
// sleepy, goodbye, and two moments of the switch bounce. Text (the name in
// the top bar, "Bye!") is not drawn here. It also prints the mean paint
// time per frame on the Mac, for comparing changes (the board is slower).

#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

#include <vector>

#include "M5Unified.h"
#include "Renderer.h"

const int fonts::FreeSansBold9pt7b = 0;
M5Stub M5;

namespace {

void put32(std::vector<uint8_t> &v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) {
        v.push_back(static_cast<uint8_t>(x >> s));
    }
}

void chunk(FILE *f, const char *kind, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> out;
    put32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(kind, kind + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, static_cast<uint32_t>(crc32(0, body.data(), static_cast<uInt>(body.size()))));
    fwrite(out.data(), 1, out.size(), f);
}

bool writePng(const char *path, int w, int h, const std::vector<uint8_t> &rgbRows) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    static const uint8_t sig[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(w));
    put32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    uLongf size = compressBound(static_cast<uLong>(rgbRows.size()));
    std::vector<uint8_t> z(size);
    compress2(z.data(), &size, rgbRows.data(), static_cast<uLong>(rgbRows.size()), 9);
    z.resize(size);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s sheet.png [time_ms]\n", argv[0]);
        return 2;
    }
    const uint32_t base = argc > 2 ? static_cast<uint32_t>(atol(argv[2])) : 10340;
    const int kW = 135, kH = 240, kScale = 2, kGap = 6;
    const int cols = 8, rows = 3;
    const int sheetW = cols * (kW * kScale + kGap) + kGap, sheetH = rows * (kH * kScale + kGap) + kGap;
    std::vector<uint8_t> sheet(static_cast<size_t>(sheetW * sheetH * 3), 0xF0);

    Renderer renderer;
    renderer.begin();
    uint64_t paintUs = 0;
    int frames = 0;

    for (int ch = 0; ch < rows; ++ch) {
        for (int col = 0; col < cols; ++col) {
            Frame f;
            f.timeMs = base;
            f.character = static_cast<uint8_t>(ch);
            f.previousCharacter = static_cast<uint8_t>((ch + 2) % 3);
            switch (col) {
            case 0:
                f.mode = Mode::Listening;
                break;
            case 1:
                f.mode = Mode::Hearing;
                f.micLevel = 200;
                break;
            case 2:
                f.mode = Mode::Thinking;
                break;
            case 3:
                f.mode = Mode::Talking;
                f.mouth = 230;
                break;
            case 4:
                f.mode = Mode::Listening;
                f.sleepy = true;
                break;
            case 5:
                f.mode = Mode::Goodbye;
                f.modeMs = 1000;
                break;
            case 6:
                f.switching = true;
                f.switchMs = 400; // falling in, stretched
                break;
            default:
                f.switching = true;
                f.switchMs = 640; // squashed on landing
                break;
            }
            renderer.draw(f);
            paintUs += renderer.lastPaintUs();
            ++frames;

            // Pull the pixels out through the snapshot path the board uses.
            struct Grab : Print {
                std::vector<uint8_t> bytes;
                size_t write(const uint8_t *b, size_t n) override {
                    bytes.insert(bytes.end(), b, b + n);
                    return n;
                }
            } grab;
            renderer.writeSnapshot(grab);
            const int ox = kGap + col * (kW * kScale + kGap), oy = kGap + ch * (kH * kScale + kGap);
            for (int y = 0; y < kH; ++y) {
                for (int x = 0; x < kW; ++x) {
                    const size_t i = static_cast<size_t>(2 * (y * kW + x));
                    const uint16_t v = static_cast<uint16_t>(grab.bytes[i] << 8 | grab.bytes[i + 1]);
                    const uint8_t r = static_cast<uint8_t>(((v >> 11) & 0x1F) << 3 | ((v >> 11) & 0x1F) >> 2);
                    const uint8_t g = static_cast<uint8_t>(((v >> 5) & 0x3F) << 2 | ((v >> 5) & 0x3F) >> 4);
                    const uint8_t b = static_cast<uint8_t>((v & 0x1F) << 3 | (v & 0x1F) >> 2);
                    for (int sy = 0; sy < kScale; ++sy) {
                        for (int sx = 0; sx < kScale; ++sx) {
                            const size_t o = static_cast<size_t>(
                                ((oy + y * kScale + sy) * sheetW + ox + x * kScale + sx) * 3);
                            sheet[o] = r;
                            sheet[o + 1] = g;
                            sheet[o + 2] = b;
                        }
                    }
                }
            }
        }
    }

    std::vector<uint8_t> rowsBuf;
    rowsBuf.reserve(static_cast<size_t>((sheetW * 3 + 1) * sheetH));
    for (int y = 0; y < sheetH; ++y) {
        rowsBuf.push_back(0);
        rowsBuf.insert(rowsBuf.end(), sheet.begin() + y * sheetW * 3, sheet.begin() + (y + 1) * sheetW * 3);
    }
    if (!writePng(argv[1], sheetW, sheetH, rowsBuf)) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    printf("%s: %d frames, %.0f us per paint on this Mac\n", argv[1], frames,
           static_cast<double>(paintUs) / frames);
    return 0;
}
