#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>

#include "AudioCapture.h"
#include "LipSync.h"
#include "PhraseRecorder.h"
#include "Renderer.h"
#include "TalkState.h"
#include "VoiceChanger.h"
#include "VoicePlayer.h"

namespace {

constexpr uint8_t kBrightness = 120;
constexpr uint32_t kFrameMs = 33;       // about 30 fps
// While thinking only the character is drawn: the voice change gets most of
// each loop, and the frame rate dips for a moment.
constexpr uint32_t kChangerBudgetUs = 45000;
constexpr uint32_t kMinThinkMs = 300;   // long enough to see the pose
constexpr uint32_t kPlaybackGraceMs = 100;

AudioCapture mic;
VoicePlayer speaker;
PhraseRecorder::Config recorderConfig;
PhraseRecorder *recorder = nullptr;
VoiceChanger changer;
LipSync lips;
TalkState state;
Renderer renderer;

// Big buffers, all in PSRAM.
int16_t *scratch = nullptr;
size_t scratchCap = 0;
int16_t *output = nullptr;
size_t outputCap = 0;

bool changing = false;
size_t lastPhraseLen = 0; // what "pin" sends; the recorder forgets it
uint32_t changeStartMs = 0;
uint32_t changeMs = 0;
uint32_t playStartMs = 0;
uint32_t micStartMs = 0;
bool waitingForMic = false;
uint32_t lastFrameMs = 0;
Mode lastMode = Mode::Listening;
bool lastSleepy = false;

// Test hooks over Serial (see "Self-testing on the board" in CLAUDE.md).
uint8_t pendingKeys = 0;
bool perfOn = false;
bool levelsOn = false;
char command[48];
size_t commandLen = 0;
struct {
    uint32_t sinceMs = 0;
    uint32_t frames = 0;
    uint32_t maxLoopUs = 0;
    uint32_t lastLevelMs = 0;
} stats;

const char *modeName(Mode m) {
    switch (m) {
    case Mode::Listening:
        return "listen";
    case Mode::Hearing:
        return "hear";
    case Mode::Thinking:
        return "think";
    case Mode::Talking:
        return "talk";
    case Mode::Goodbye:
        return "bye";
    }
    return "?";
}

int16_t *psramBuffer(size_t samples) {
    return static_cast<int16_t *>(heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
}

void startMic(uint32_t now) {
    recorder->reset();
    mic.start();
    micStartMs = now;
    waitingForMic = true;
}

void startChanging(uint32_t now) {
    lastPhraseLen = recorder->length();
    const Voice &voice = voices::forCharacter(state.character());
    changer.start(recorder->samples(), recorder->length(), scratch, scratchCap, output, outputCap,
                  voice);
    changing = true;
    changeStartMs = now;
}

void powerOff() {
    Serial.println("EV off");
    Serial.flush();
    M5.Display.setBrightness(0);
    M5.Power.powerOff();
    // Still running: USB power can keep the board on. Sleep with no way to
    // wake up but a press of the side button.
    delay(1000);
    Serial.println("EV still on, deep sleep");
    Serial.flush();
    M5.Display.sleep();
    esp_deep_sleep_start();
}

void execute(uint8_t actions, uint32_t now) {
    if (actions & TalkState::kStopPlayback) {
        speaker.stop();
    }
    if (actions & TalkState::kStopMic) {
        mic.stop();
    }
    if (actions & TalkState::kProcess) {
        startChanging(now);
    }
    if (actions & TalkState::kPlay) {
        speaker.play(output, changer.length(), VoiceChanger::kSampleRate);
        playStartMs = now;
        Serial.printf("EV play start %u\n", static_cast<unsigned>(changer.length()));
    }
    if (actions & TalkState::kStartMic) {
        speaker.stop();
        startMic(now);
    }
    if (actions & TalkState::kPowerOff) {
        powerOff();
    }
}

void sendPcm(const char *which, const int16_t *samples, size_t len) {
    Serial.printf("PCM %s %u %u\n", which, static_cast<unsigned>(len),
                  static_cast<unsigned>(VoiceChanger::kSampleRate));
    // The ESP32 is little-endian, like the WAV files the Mac makes of it.
    Serial.write(reinterpret_cast<const uint8_t *>(samples), len * sizeof(int16_t));
    Serial.flush();
}

// Test hook: changes the last phrase with every voice in one go and times
// each pass. Only while listening, when the buffers are free.
void bench() {
    if (state.mode() != Mode::Listening || lastPhraseLen == 0) {
        Serial.println("ERR bench needs a phrase and listening");
        return;
    }
    for (uint8_t c = 0; c < voices::kCount; ++c) {
        uint32_t us[6] = {};
        changer.start(recorder->samples(), lastPhraseLen, scratch, scratchCap, output, outputCap,
                      voices::forCharacter(c));
        while (!changer.done()) {
            const int stage = changer.stage();
            const uint32_t t0 = micros();
            changer.step(1);
            us[stage] += micros() - t0;
        }
        Serial.printf("BENCH char %u in %u out %u stretch %u filter %u resample %u finish %u us\n",
                      c, static_cast<unsigned>(lastPhraseLen),
                      static_cast<unsigned>(changer.length()), static_cast<unsigned>(us[1]),
                      static_cast<unsigned>(us[2]), static_cast<unsigned>(us[3]),
                      static_cast<unsigned>(us[4]));
    }
}

void printStatus(uint32_t now) {
    Serial.printf("ST mode %s char %u idle %u timeout %u noise %.1f level %.1f lost %u\n",
                  modeName(state.mode()), state.character(),
                  static_cast<unsigned>(state.idleFor(now)),
                  static_cast<unsigned>(state.idleTimeout()), recorder->noiseFloor(),
                  recorder->level(), static_cast<unsigned>(mic.lostBlocks()));
}

void runCommand(const char *line, uint32_t now) {
    if (strcmp(line, "k") == 0) {
        ++pendingKeys;
    } else if (strncmp(line, "c ", 2) == 0) {
        const int want = atoi(line + 2) % TalkState::kCharacters;
        pendingKeys = static_cast<uint8_t>((want - state.character() + TalkState::kCharacters) %
                                           TalkState::kCharacters);
    } else if (strcmp(line, "s") == 0) {
        renderer.writeSnapshot(Serial);
    } else if (strcmp(line, "pin") == 0) {
        sendPcm("in", recorder->samples(), lastPhraseLen);
    } else if (strcmp(line, "pout") == 0) {
        sendPcm("out", output, changer.length());
    } else if (strncmp(line, "idle ", 5) == 0) {
        state.setIdleTimeout(static_cast<uint32_t>(atol(line + 5)) * 1000);
        Serial.printf("OK idle %u\n", static_cast<unsigned>(state.idleTimeout()));
    } else if (strcmp(line, "bench") == 0) {
        bench();
    } else if (strcmp(line, "st") == 0) {
        printStatus(now);
    } else if (strcmp(line, "perf") == 0) {
        perfOn = !perfOn;
    } else if (strcmp(line, "lv") == 0) {
        levelsOn = !levelsOn;
    } else if (line[0] != '\0') {
        Serial.printf("ERR unknown '%s'\n", line);
    }
}

void pollSerial(uint32_t now) {
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == '\n' || c == '\r') {
            command[commandLen] = '\0';
            runCommand(command, now);
            commandLen = 0;
        } else if (commandLen + 1 < sizeof command) {
            command[commandLen++] = static_cast<char>(c);
        }
    }
}

uint8_t micLevel() {
    // -50 dBFS and below is nothing, -15 dBFS is shouting.
    const float l = (recorder->level() + 50.0f) / 35.0f;
    return static_cast<uint8_t>(255.0f * (l < 0 ? 0 : (l > 1 ? 1 : l)));
}

void drawFrame(uint32_t now) {
    Frame f;
    f.timeMs = now;
    f.mode = state.mode();
    f.modeMs = now - state.modeSinceMs();
    f.character = state.character();
    f.previousCharacter = state.previousCharacter();
    f.switchMs = now - state.switchedAtMs();
    f.switching = state.hasSwitched() && f.switchMs < Renderer::kSwitchMs;
    if (f.mode == Mode::Talking) {
        f.mouth = lips.openness(now - playStartMs);
    }
    if (f.mode == Mode::Hearing) {
        f.micLevel = micLevel();
    }
    f.sleepy = state.sleepy(now);
    renderer.draw(f);
    ++stats.frames;
}

void reportEvents(uint32_t now) {
    const Mode mode = state.mode();
    const bool sleepy = state.sleepy(now);
    if (mode != lastMode) {
        Serial.printf("EV %s char %u\n", modeName(mode), state.character());
        lastMode = mode;
    }
    if (sleepy != lastSleepy) {
        Serial.printf("EV sleepy %u\n", sleepy ? 1 : 0);
        lastSleepy = sleepy;
    }
    if (perfOn && now - stats.sinceMs >= 1000) {
        Serial.printf("perf paint %u push %u fps %u loop %u lost %u\n",
                      static_cast<unsigned>(renderer.lastPaintUs()),
                      static_cast<unsigned>(renderer.lastPushUs()),
                      static_cast<unsigned>(stats.frames * 1000 / (now - stats.sinceMs)),
                      static_cast<unsigned>(stats.maxLoopUs),
                      static_cast<unsigned>(mic.lostBlocks()));
        stats.sinceMs = now;
        stats.frames = 0;
        stats.maxLoopUs = 0;
    }
    if (levelsOn && now - stats.lastLevelMs >= 100) {
        Serial.printf("LV %.1f %.1f\n", recorder->level(), recorder->noiseFloor());
        stats.lastLevelMs = now;
    }
}

void halt(const char *why) {
    Serial.printf("ERR %s\n", why);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.drawString(why, M5.Display.width() / 2, M5.Display.height() / 2);
    for (;;) {
        delay(1000);
    }
}

} // namespace

void setup() {
    auto cfg = M5.config();
    // The mic and the speaker share the I2S lines; both are configured
    // here and take turns later.
    cfg.internal_mic = true;
    cfg.internal_spk = true;
    M5.begin(cfg);
    Serial.begin(115200);

    M5.Display.setBrightness(kBrightness);
    renderer.begin();
    speaker.begin();

    const size_t phraseCap = PhraseRecorder::capacityFor(recorderConfig);
    int16_t *phrase = psramBuffer(phraseCap);
    for (uint8_t c = 0; c < voices::kCount; ++c) {
        const Voice &v = voices::forCharacter(c);
        scratchCap = std::max(scratchCap, VoiceChanger::stretchCapacity(phraseCap, v));
        outputCap = std::max(outputCap, VoiceChanger::outputCapacity(phraseCap, v));
    }
    scratch = psramBuffer(scratchCap);
    output = psramBuffer(outputCap);
    if (phrase == nullptr || scratch == nullptr || output == nullptr) {
        halt("no memory");
    }
    recorder = new PhraseRecorder(phrase, phraseCap, recorderConfig);
    Serial.printf("EV boot phrase %u scratch %u output %u samples\n",
                  static_cast<unsigned>(phraseCap), static_cast<unsigned>(scratchCap),
                  static_cast<unsigned>(outputCap));

    if (!mic.begin()) {
        halt("no microphone");
    }
    const uint32_t now = millis();
    state.begin(now);
    startMic(now);
    stats.sinceMs = now;
    Serial.println("EV listen char 0");
}

void loop() {
    const uint32_t loopStart = micros();
    M5.update();
    const uint32_t now = millis();
    pollSerial(now);

    TalkState::Input in;
    // KEY1 is the blue button. KEY2 and the side (power) button do nothing
    // here: the PMIC handles the side button by itself.
    in.key1 = M5.BtnA.wasPressed();
    if (!in.key1 && pendingKeys > 0) {
        --pendingKeys;
        in.key1 = true;
    }

    while (mic.running()) {
        const int16_t *block = mic.next();
        if (block == nullptr) {
            break;
        }
        const PhraseRecorder::Event e = recorder->push(block);
        if (waitingForMic && recorder->level() > -119.0f) {
            waitingForMic = false;
            Serial.printf("EV mic ready %u\n", static_cast<unsigned>(now - micStartMs));
        }
        if (e == PhraseRecorder::Event::Started) {
            in.phraseStarted = true;
            Serial.printf("EV heard start noise %.1f level %.1f\n", recorder->noiseFloor(),
                          recorder->level());
        } else if (e == PhraseRecorder::Event::Discarded) {
            in.phraseDiscarded = true;
            Serial.println("EV heard discard");
        } else if (e == PhraseRecorder::Event::Ended) {
            in.phraseEnded = true;
            Serial.printf("EV heard end %u ms %u samples peak %.1f\n",
                          static_cast<unsigned>(recorder->lengthMs()),
                          static_cast<unsigned>(recorder->length()), recorder->peakDbfs());
            break; // the phrase is complete; the mic stops next
        }
    }

    if (changing) {
        const uint32_t t0 = micros();
        while (!changer.done() && micros() - t0 < kChangerBudgetUs) {
            changer.step(4);
        }
        if (changer.done()) {
            changing = false;
            changeMs = now - changeStartMs;
            lips.analyze(output, changer.length(), VoiceChanger::kSampleRate);
            Serial.printf("EV fx char %u %u ms %u samples\n", state.character(),
                          static_cast<unsigned>(changeMs),
                          static_cast<unsigned>(changer.length()));
        }
    }
    if (state.mode() == Mode::Thinking && !changing && now - state.modeSinceMs() >= kMinThinkMs) {
        in.processed = true;
    }
    if (state.mode() == Mode::Talking && now - playStartMs >= kPlaybackGraceMs &&
        !speaker.playing()) {
        in.playbackDone = true;
        Serial.printf("EV play end %u ms\n", static_cast<unsigned>(now - playStartMs));
    }

    const uint8_t actions = state.update(now, in);
    if (in.key1) {
        Serial.printf("EV key1 char %u\n", state.character());
    }
    execute(actions, now);
    reportEvents(now);

    if (now - lastFrameMs >= kFrameMs) {
        lastFrameMs = now;
        drawFrame(now);
    }

    stats.maxLoopUs = std::max<uint32_t>(stats.maxLoopUs, micros() - loopStart);
    delay(1);
}
