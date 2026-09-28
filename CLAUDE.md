# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

A Talking Tom–style toy for the M5StickS3: a cartoon character listens to a
phrase, then repeats it in a funny voice, moving its mouth and paws. There are
three characters, switched with KEY1 (the blue button): the Cat (a cartoon
voice, like Talking Tom), the Hippo (bass) and the Mouse (squeaky). The toy
starts with the Cat. After two minutes without a phrase it gets sleepy, says
goodbye and powers off. KEY2 and the side button are not used by the firmware
(the side button belongs to the PMIC).

Repository: https://github.com/mbogatyr/M5TalkingTom

## Language

All documentation and the project itself are kept in English: this file, code
comments, identifiers, strings shown on the display, commit messages and any
other text files. New text is written in English too, even when the
conversation with the user happens in another language.

## Commands

PlatformIO is not installed globally but by the official installer into a
venv. The binary lives at `~/.platformio/penv/bin/pio`; it is not on `PATH`,
so it has to be called by its full path.

```bash
~/.platformio/penv/bin/pio test -e native                       # host unit tests for the logic
~/.platformio/penv/bin/pio test -e native -f test_voice_changer # a single test suite
~/.platformio/penv/bin/pio run -e sticks3                       # build the firmware
~/.platformio/penv/bin/pio run -e sticks3 -t upload             # flash the board
~/.platformio/penv/bin/pio run -e sticks3 -t merged             # single image for M5Burner
~/.platformio/penv/bin/python tools/talk_test.py --mic          # end-to-end test on the board
```

The xtensa-esp32s3 toolchain is installed into `~/.platformio/packages` once
per machine and is shared by all projects.

**After every upload send `idle 0`** (`tools/serial_cmd.py send "idle 0"`):
otherwise the toy powers itself off after two quiet minutes, even on USB, and
only a press of the side button brings it back. It lasts until the next
reset.

## How it works

```
Listening --phrase starts--> Hearing --0.7 s of quiet--> Thinking --> Talking --> Listening
 (mic on)                    (recording)   mic off       (voice change) speaker on,
                                                                        playback, then
                                                                        speaker off, mic on
```

The mic and the speaker share the I2S clock lines and take turns (the same
way M5Unified's `examples/Basic/Microphone` does it): `M5.Mic.end()` before
`M5.Speaker.begin()`, and back. After `Mic.begin()` the ES8311 gives zeros
for about 70 ms (measured; M5VoiceRecorder saw up to a second after a cold
power-up), which the phrase recorder ignores.

KEY1 in each mode: Listening — next character; Hearing — next character, the
recording goes on; Thinking — next character, the voice change restarts with
the new voice; Talking — playback stops, next character, back to listening;
Goodbye — cancels the goodbye (no switch). A KEY1 press restarts the idle
clock, like a phrase does; the clock only runs while listening.

## Architecture

The split into `lib/` and `src/` is not cosmetic here, it is load-bearing:

- `lib/` is the logic: plain C++ with no Arduino, no M5Unified and no hardware
  access of any kind.
  - `PhraseRecorder` — finds a phrase in 512-sample blocks (32 ms at 16 kHz):
    RMS level against a noise floor that falls fast and rises 0.05 dB per
    block; start at max(noise + 15 dB, -45 dBFS) for two blocks, keep going
    at max(noise + 9 dB, -50 dBFS); 320 ms pre-roll, ends after 700 ms of
    quiet keeping a 150 ms tail, drops phrases under 350 ms, cuts at 8 s;
    ignores all-zero blocks and only learns the noise for 250 ms after a
    restart. The buffer belongs to the caller (PSRAM).
  - `VoiceChanger` — pitch shift at a kept tempo: WSOLA stretch by
    pitch / tempo (32 ms Hann windows, 50 % overlap, ±8 ms search, coarse on
    every 4th lag and 8th sample, then fine), an 80 Hz high-pass, a 4th-order
    Butterworth low-pass when the pitch goes up, resampling by the pitch with
    vibrato, optional soft clipping (a Padé tanh), normalising to -1 dBFS
    with at most +20 dB, 8 ms fades. Works in small `step()`s so frames keep
    coming. The voices (`voices::` in `VoiceChanger.h`):

    | Character | pitch | tempo | extra |
    |---|---|---|---|
    | Cat | 1.6 | 1.05 | 5 Hz vibrato, 2 % |
    | Hippo | 0.62 | 0.9 | soft clipping, drive 2.5 |
    | Mouse | 2.2 | 1.2 | 7 Hz vibrato, 3 % |

    The formants move with the pitch, which is what gives the cartoon
    timbre. The ESP32-S3's FPU is single precision only: no `double` in the
    sample loops (it made the change four times slower). The file is built
    with `#pragma GCC optimize("O2")`; the core's `-Os` is twice as slow.
  - `LipSync` — the finished phrase as mouth openings per 20 ms (-40..-12
    dBFS mapped to closed..open, opens at once, closes by 70/255 a frame).
  - `TalkState` — the modes above, the character, the switch time, the idle
    clock (sleepy for the last 20 s of 120 s, then Goodbye for 3 s, then
    power off). `setIdleTimeout(0)` never powers off.
  - `BlockRing` — which mic blocks are finished (from M5VoiceRecorder), with
    `reset()` for the mic restarts.
- `src/` is everything that knows about the board: `AudioCapture` (mic at
  16 kHz), `VoicePlayer` (speaker at 48 kHz, volume 255, magnification 4:
  see "Loudness" below), `Painter` and
  `Renderer` (the display), `main.cpp` (wiring, serial events and test
  commands, power off).

The `native` environment builds only `lib/` (PlatformIO's `test_build_src`
defaults to `no`), so the logic is tested on the Mac without the board.
**Do not pull hardware dependencies into `lib/`: that breaks the tests.**

### Time is passed in as a parameter

The logic in `lib/` does not call `millis()` itself; it receives the current
time as an argument (the recorder counts blocks instead). `millis()`
overflows after roughly 49 days: intervals are computed with unsigned
subtraction `now - since`, so the overflow goes unnoticed.

### Rendering

The look is designed in `tools/character_prototype.html` (open it in a
browser): the three characters at 135x240 with buttons for every state and
the switch. **Change the look there first**, then port the change; the C++ has
the same shape calls, coordinates and colours.

`Painter` draws anti-aliased ellipses, triangles, capsules (wide lines),
round rects and arcs straight into the sprite buffer, coverage from the
distance to the edge (LovyanGFX fills ellipses and triangles without
anti-aliasing). The cartoon outline is `part()`: the group of shapes grown by
2 px in ink, then filled. A head tilt rotates the anchor points around the
neck; the switch bounce moves and squashes everything around the floor line.
The buffer is byte-swapped RGB565, like any 16-bit LovyanGFX sprite.

`Renderer` composes the frame in an `M5Canvas` (PSRAM) and pushes it with
one `pushSprite` (about 14.5 ms); `main.cpp` draws every 33 ms. The display
is portrait, KEY1 below the screen. Text is a LovyanGFX font
(`FreeSansBold9pt7b`): the name in the top bar and "Bye!"; the floating "z"
are drawn as lines.

`tools/render_preview/` builds the real `Renderer` and `Painter` on the Mac
with a tiny M5Unified stand-in and saves a contact sheet of every state
(no text), to check a change without the board:

```bash
c++ -std=gnu++17 -O2 -Itools/render_preview -Isrc -Ilib/TalkState \
    tools/render_preview/main.cpp src/Renderer.cpp src/Painter.cpp -lz -o /tmp/render_preview
/tmp/render_preview /tmp/sheet.png
```

## Self-testing on the board

Testing is automatic, with no one speaking: the Mac says test phrases through
its own speaker (`say -v Milena` / `say -v Samantha`, played with `afplay`),
the board next to it hears them and answers, and a script checks the result
over serial.

```bash
PY=~/.platformio/penv/bin/python
$PY tools/talk_test.py --mic --out /tmp/talk   # all characters; WAVs + screenshots
$PY tools/talk_test.py --quick --chars 2       # two phrases, the mouse only
$PY tools/serial_cmd.py monitor 10             # print what the board says
$PY tools/serial_cmd.py snap screen.png
$PY tools/serial_cmd.py pcm out said.wav       # the last phrase as the board said it
```

`talk_test.py` checks: no false starts in 10 s of silence; a 60 ms beep is
dropped; ≥ 25 fps; for each character and phrase (Russian, English, pauses
between words, quiet, short, a 12 s monologue) the phrase is caught, its end
noticed 0.5–1.1 s after the Mac's last sound, its length right (the 8 s cut
for the monologue), the voice change under 800 ms, the playback as long as
the sound, and the pitch (autocorrelation, within 8 %) and tempo of what the
board said. With `--mic` the Mac's microphone (ffmpeg, avfoundation) records
the board's speaker, and the sound in the air must be loud and have the
character's pitch within 12 %. The terminal needs microphone access (the user
granted it on 2026-09-28; without it ffmpeg hangs waiting for the prompt).
`tools/audio_analysis.py` has the WAV, level and pitch helpers (plain
Python; numpy only speeds it up). `tools/voice_preview.cpp` runs the voices
on a WAV on the Mac.

Serial protocol (115200, lines). Events: `EV listen|hear|think|talk|bye
char <n>`, `EV heard start|discard|end ...`, `EV mic ready <ms>`,
`EV fx char <n> <ms> ms <samples> samples`, `EV play start <samples>`,
`EV play end <ms> ms`, `EV key1 char <n>`, `EV sleepy 0|1`, `EV off`.
Commands: `k` (KEY1), `c <n>` (character), `s` (screenshot, `SNAP w h` +
RGB565), `pin` / `pout` (the last phrase heard / said, `PCM <which> <n>
<rate>` + int16 LE), `idle <s>` (0 = never power off), `st` (status), `perf`
(a line a second: paint, push, fps, loop, lost blocks), `lv` (mic levels
every 100 ms), `bench` (times each voice change pass on the last phrase),
`vol <0-255>` and `mag <n>` (speaker volume and M5Unified magnification until
the next reset), `replay` (says the last phrase again; blocks the loop).

Results on 2026-09-28 (board 20–30 cm from the MacBook's speaker): room noise
-48…-53 dBFS, the Mac's speech -24 dBFS at the start of a phrase, peaks -13
dBFS; all 154 checks of `talk_test.py --mic` pass; the voice change takes
about 250-330 ms for a 4.5 s phrase, 470-600 ms for 8 s; the toy answers about 0.9 s after the speaker goes quiet
(0.7 s of that is the wait for more speech). Pitch on the board: cat ×1.6,
hippo ×0.62, mouse ×2.14–2.18.

**Loudness.** M5Unified's settings for the StickS3 speaker (magnification
1) play a -1 dBFS phrase about 20 dB below what the speaker can do.
Measured with the Mac's microphone (`vol`, `mag` and `replay` commands):
+6 dB per doubling of the magnification up to 4, only +4.8 dB at 8 (the
peaks clip). So `VoicePlayer` uses volume 255 and magnification 4, and
`VoiceChanger` adds 8 dB with a peak limiter. The board then sounds at
-20…-30 dBFS on the Mac's microphone, about as loud as the Mac's own speaker
(-24 dBFS), instead of -55 dBFS. The tiny speaker hardly plays anything
below ~200 Hz: the hippo's bass comes across through its harmonics (the
soft clipping adds more of them), and its pitch in the air cannot be
measured reliably.

**Opening the port.** The USB-Serial-JTAG resets the chip while RTS is high
and DTR is low; `serial_cmd.open_port()` (from M5VoiceRecorder) keeps DTR
high until RTS is low, and the board does not reset. Use it for any script
that talks to the board. Right after the port opens a reply can get stuck in
the USB buffer until the next open; `talk_test.py` asks for the status again
when a reply is late.

## Board specifics

M5StickS3 is an ESP32-S3-PICO-1-N8R8 with 8 MB of flash, 8 MB of octal PSRAM,
an ST7789P3 135x240 display, an ES8311 codec with a MEMS microphone and a
small speaker behind an amp that the PMIC switches.

- PlatformIO has **no** `m5stack-sticks3` board id. The project uses
  `esp32-s3-devkitc-1` plus `board_build.arduino.memory_type = qio_opi` and
  the `default_8MB.csv` partitions. Do not "fix" this to a non-existent id.
- USB is native, with no CH9102 bridge, so on macOS the port is called
  `/dev/cu.usbmodem*`, not `/dev/cu.usbserial*`. Serial output needs the
  `-DARDUINO_USB_CDC_ON_BOOT=1` flag, which is already set.
- Buttons: KEY1 on G11 (`M5.BtnA`), KEY2 on G12 (`M5.BtnB`). Grove (G9/G10)
  and HAT2 (G1–G8, G43, G44) are free.
- Buffers: the phrase (134 144 samples), the stretched phrase and the output
  are allocated in PSRAM with `heap_caps_malloc(MALLOC_CAP_SPIRAM)`, about
  1 MB together.

### Publishing to M5Burner

M5Burner writes the uploaded file starting at address 0x0, so it needs a full
image. A bare `firmware.bin` is meant for address 0x10000: written at 0x0, it
overwrites the bootloader. `pio run -e sticks3 -t merged` (the extra script
`tools/merged_image.py`) merges the bootloader, the partition table,
`boot_app0` and the application into `.pio/build/sticks3/firmware-merged.bin`
with esptool `merge_bin`. The script takes the addresses and flash parameters
(dio, 80m, 8MB) from PlatformIO's regular upload settings, so the image matches
what `upload` writes.

How this is known. Checked in M5SpectrumAnalyzer on 2026-09-27: of the six
StickS3 firmwares on burner.m5stack.com, five, including the official
UIFlow2.0, are full images. Each has the bootloader at 0x0 (header
`e9 03 02 3f`), the partition table at 0x8000 (`aa 50`) and the application at
0x10000. One firmware was uploaded as a bare application.

The upload form is at burner.m5stack.com/developer/firmware/upload. It asks for:
- a name, a category and the supported devices (StickS3);
- a firmware description and a version description, both in Markdown;
- the version number and a link to the project (the repository above);
- the `.bin` file;
- visibility: Public requires moderation;
- a cover image: a screenshot of the screen works.

The upload is done through Claude in Chrome, where the user is signed in and
files can be attached (the built-in browser can't attach files). Copy the
image and the cover into `dist/` first (ignored by git):
`dist/M5TalkingTom-v<version>.bin` and `dist/M5TalkingTom-cover.png`. The
cover is 1440x810 (the shape M5Burner shows): the three characters'
listening screens from the board, 3x with nearest-neighbour, side by side on
the top bar's colour `#1B1720`.

v1.0.0 was uploaded on 2026-09-28 as "Talking Tom" (the name the user chose;
"Talking Tom" is Outfit7's trademark, which moderation may object to),
category Games, StickS3, Public, and went to review (Pending). Before the
upload the merged image was written on its own at 0x0 with esptool
`write_flash 0x0`, the way M5Burner writes it, and the board booted into it.

### Powering off

The side button is handled by the PMIC, not the firmware:

| Action | Result |
|---|---|
| Single press | Power on / reset |
| Double press | Power off |
| Long hold | Download mode (the internal green LED blinks) |

The idle power-off calls `M5.Power.powerOff()` (M5Unified 0.2.23, which
fixed the StickS3 waking up again by timer,
[M5Unified#235](https://github.com/m5stack/M5Unified/issues/235)). If the
board is still running a second later, it turns the display off and goes
into deep sleep with no wake-up source. Checked on 2026-09-28 with the board
plugged into the Mac (twice: after two quiet minutes, and with `idle 8`): it
printed `EV off` but never `EV still on`, its USB port disappeared and it
stayed off until the side button was pressed. So `M5.Power.powerOff()` cuts
the power itself, on USB power too, and the board does not wake up by
itself.

Testing the goodbye without losing the board: the idle clock counts from the
last activity (a phrase, a KEY1 press), not from the `idle` command, and the
goodbye lasts 3 s before the power goes. Send `k` within those 3 s to cancel
it.

### If flashing fails

`A fatal error occurred: Failed to connect to ESP32-S3: No serial data received.`

The board shows up as `USB JTAG_serial debug unit` (VID 0x303A, PID 0x1001):
that is the built-in USB-Serial-JTAG, not a CDC port (a consequence of
`ARDUINO_USB_MODE=1`). Auto-reset into download mode through it does not
always work. The fix is manual: hold the side button until the green LED
blinks. (In this project every upload worked automatically.) If there is no
port at all, the toy has powered itself off: press the side button once.

### If the board is stuck in the bootloader

The firmware does not start, and the port shows `boot:0x0 (DOWNLOAD(USB/UART0))`
and `waiting for download`. The way out is a single short press of the side
button. It can be caused by a script toggling DTR/RTS on the port; see
"Opening the port" above.

## Tests

Unit tests cover the logic in `lib/`, with one directory
`test/test_<module>/test_main.cpp` per module. Unity is built without double
precision: use the `FLOAT` assertions. Rendering is checked by eye (the
prototype, `tools/render_preview`, screenshots from the board): do not try to
write tests for `Renderer`.

`main` in the tests returns the number of failures from `UNITY_END()`, and
PlatformIO reports a non-zero exit code as a signal number. A line like
`Program received signal SIGALRM` with failing tests is a reporting artifact,
not a separate problem; it disappears once the tests pass.
