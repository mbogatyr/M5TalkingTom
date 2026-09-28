"""End-to-end self-test of the toy, with no one speaking.

The Mac speaks test phrases through its own speaker (`say`, then `afplay`),
the StickS3 next to it hears them, changes the voice and plays it back.
The script follows the board's events over serial, fetches what the board
heard and what it said, and checks:

  - every phrase is caught, and nothing is caught in silence;
  - the end of a phrase is noticed within a second of the Mac going quiet;
  - the phrase the board kept is as long as the one the Mac said;
  - clicks too short to be a phrase are dropped, a monologue is cut at 8 s;
  - the changed voice has the character's pitch (ratio within 8 %) and
    tempo, and the playback lasts as long as it should;
  - how long the voice change took, and the frame rate;
  - with --mic, the Mac's microphone records the board's speaker, and the
    sound in the air must be loud and have the character's pitch too.

    PY=~/.platformio/penv/bin/python
    $PY tools/talk_test.py                  # all three characters
    $PY tools/talk_test.py --chars 1 --quick
    $PY tools/talk_test.py --out /tmp/talk  # keeps WAVs and screenshots
    $PY tools/talk_test.py --mic            # also listens with the Mac's mic

Put the board 10-30 cm from the Mac's speaker, with the Mac's volume at a
normal level. Needs pyserial (ships with PlatformIO); numpy only speeds up
the pitch analysis; --mic needs ffmpeg and microphone access for the
terminal.
"""
import argparse
import math
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_analysis as aa  # noqa: E402
import serial_cmd as sc  # noqa: E402

NAMES = ["cat", "hippo", "mouse"]
# Must match lib/VoiceChanger/VoiceChanger.h.
PITCH = [1.6, 0.62, 2.2]
TEMPO = [1.05, 0.9, 1.2]
MAX_PHRASE_S = 8.0

# (name, voice, text, afplay volume, what should happen)
PHRASES = [
    ("ru", "Milena", "Привет! Я говорящий кот. Скажи мне что-нибудь, и я повторю.", 1.0, "phrase"),
    ("en", "Samantha", "Hello there. I am a talking cat, and I repeat everything you say.", 1.0, "phrase"),
    ("pauses", "Samantha", "One, [[slnc 380]] two, [[slnc 380]] three, and four.", 1.0, "phrase"),
    ("quiet", "Milena", "Тише едешь, дальше будешь.", 0.35, "phrase"),
    ("short", "Milena", "Привет!", 1.0, "phrase"),
    ("long", "Samantha", "This is a very long monologue that goes on and on, much longer than "
     "the toy is willing to listen to, because a talking toy should answer quickly "
     "instead of waiting forever for the speaker to finish every single thought.", 1.0, "cut"),
]
QUICK = ["ru", "en"]


class Board:
    def __init__(self, verbose):
        self.link = sc.open_port()
        self.t0 = time.time()
        self.events = []  # (time, line)
        self.verbose = verbose
        self.reader = sc.Reader(self.link, echo=self._seen)

    def _seen(self, line):
        now = time.time()
        if line.startswith("EV ") or line.startswith("perf ") or line.startswith("ERR"):
            self.events.append((now, line))
            if self.verbose:
                print("   %7.2f | %s" % (now - self.t0, line))

    def send(self, line):
        sc.send(self.link, line)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.reader.line(end)

    def wait_for(self, prefix, timeout, since=None):
        """Time and text of the first event starting with prefix after
        `since` (default: now)."""
        since = time.time() if since is None else since
        end = time.time() + timeout
        while True:
            for t, line in self.events:
                if t >= since and line.startswith(prefix):
                    return t, line
            if time.time() > end:
                return None, None
            self.reader.line(min(end, time.time() + 0.05))

    def status(self):
        """The board's status line. Right after the port opens a reply can
        get stuck in the USB buffer, so it asks again when one is late."""
        for _ in range(3):
            self.send("st")
            line = self.reader.line(time.time() + 1.0, want="ST ")
            if line:
                return line
        return None

    def pcm(self, which):
        self.reader.echo = None
        try:
            return sc.get_pcm(self.link, self.reader, which)
        finally:
            self.reader.echo = self._seen

    def snap(self, path):
        self.reader.echo = None
        try:
            sc.snap(self.link, self.reader, path, scale=2)
        finally:
            self.reader.echo = self._seen


def find_mac_mic():
    """ffmpeg's avfoundation index of the built-in microphone."""
    out = subprocess.run(["ffmpeg", "-hide_banner", "-f", "avfoundation", "-list_devices", "true",
                          "-i", ""], capture_output=True, text=True).stderr
    audio = out.split("audio devices:")[-1]
    for line in audio.splitlines():
        if "MacBook" in line and "Microphone" in line:
            return line.split("[")[2].split("]")[0]
    return "0"


def record_mac(mic, path, seconds):
    return subprocess.Popen(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "avfoundation",
                             "-i", ":" + mic, "-t", "%.1f" % seconds, "-ac", "1", "-ar", "16000",
                             "-y", path])


def synthesize(tmp, name, voice, text):
    aiff = os.path.join(tmp, name + ".aiff")
    wav = os.path.join(tmp, name + ".wav")
    if not os.path.exists(wav):
        subprocess.run(["say", "-v", voice, "-o", aiff, text], check=True)
        subprocess.run(["afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", aiff, wav],
                       check=True)
    samples, rate = aa.read_wav(wav)
    return aiff, samples, rate


def ensure_listening(board, timeout=20):
    """Waits until the board is listening and has been for a moment."""
    end = time.time() + timeout
    while time.time() < end:
        st = board.status()
        if st and " mode listen " in st:
            board.pump(1.0)
            return True
        board.pump(0.5)
    return False


def select(board, char):
    board.send("c %d" % char)
    board.pump(1.5)  # the switch animation
    st = board.status()
    return st is not None and (" char %d " % char) in st


class Checks:
    def __init__(self):
        self.rows = []
        self.failed = 0

    def check(self, label, ok, detail):
        self.rows.append((label, ok, detail))
        if not ok:
            self.failed += 1
        print("   %s %-34s %s" % ("ok  " if ok else "FAIL", label, detail))


def run_phrase(board, checks, char, tmp, out, case, mic=None):
    name, voice, text, volume, expect = case
    label = "%s/%s" % (NAMES[char], name)
    aiff, said, rate = synthesize(tmp, name, voice, text)
    voice_from, voice_to = aa.voiced_bounds(said, rate)
    said_span = voice_to - voice_from
    if not ensure_listening(board):
        checks.check(label + " listening", False, "board never got back to listening")
        return

    start = time.time()
    player = subprocess.Popen(["afplay", "-v", str(volume), aiff])
    t_heard, _ = board.wait_for("EV heard start", 5, since=start)
    player.wait()
    t_end, end_line = board.wait_for("EV heard end", 4, since=start)
    if t_heard is None or t_end is None:
        checks.check(label + " caught", False, "no heard start/end (start %s)" % t_heard)
        return
    kept_ms = int(end_line.split()[3])
    recorder = None
    air = os.path.join(tmp, "air.wav")
    if mic is not None:
        # From now until the playback is surely over.
        if os.path.exists(air):
            os.remove(air)
        recorder = record_mac(mic, air, kept_ms / 1000.0 / TEMPO[char] + 2.0)
    # From the Mac's last loud sound (afplay starts about 0.1 s late).
    lag = t_end - (start + voice_to)
    if expect == "cut":
        checks.check(label + " cut at 8 s", abs(kept_ms / 1000.0 - (MAX_PHRASE_S + 0.3)) < 0.5,
                     "kept %.2f s of %.2f s" % (kept_ms / 1000.0, said_span))
    else:
        checks.check(label + " end noticed", 0.5 <= lag <= 1.1,
                     "%.2f s after the Mac went quiet" % lag)
        # The recorder keeps up to 0.32 s before and 0.15 s after the voice.
        diff = kept_ms / 1000.0 - said_span
        checks.check(label + " length", -0.3 <= diff <= 0.6,
                     "kept %.2f s, the Mac said %.2f s" % (kept_ms / 1000.0, said_span))

    t_fx, fx_line = board.wait_for("EV fx", 10, since=start)
    t_play, play_line = board.wait_for("EV play start", 10, since=start)
    if out and t_play is not None:
        board.pump(0.8)
        board.snap(os.path.join(out, "%s_talk.png" % label.replace("/", "_")))
    t_done, done_line = board.wait_for("EV play end", 30, since=start)
    if t_fx is None or t_done is None:
        checks.check(label + " said back", False, "fx %s, play end %s" % (fx_line, done_line))
        return
    fx_ms = int(fx_line.split()[4])
    out_samples = int(play_line.split()[3])
    play_ms = int(done_line.split()[3])
    checks.check(label + " voice change time", fx_ms <= 800,
                 "%d ms for %.2f s" % (fx_ms, kept_ms / 1000.0))
    expected_s = out_samples / 16000.0
    checks.check(label + " playback", abs(play_ms / 1000.0 - expected_s) < 0.35,
                 "%.2f s for %.2f s of sound" % (play_ms / 1000.0, expected_s))

    if recorder is not None:
        recorder.wait()
    ensure_listening(board)
    heard, r1 = board.pcm("in")
    changed, r2 = board.pcm("out")
    ratio, f_in, f_out = aa.pitch_ratio(heard, changed, r1)
    want = PITCH[char]
    checks.check(label + " pitch", ratio > 0 and abs(ratio / want - 1) <= 0.08,
                 "x%.2f (want x%.2f): %.0f -> %.0f Hz" % (ratio, want, f_in, f_out))
    tempo = len(heard) / max(1, len(changed))
    checks.check(label + " tempo", abs(tempo / TEMPO[char] - 1) <= 0.05,
                 "x%.2f (want x%.2f)" % (tempo, TEMPO[char]))
    checks.check(label + " loudness", aa.peak_dbfs(changed) > -3,
                 "heard peak %.1f dBFS, said peak %.1f dBFS" % (aa.peak_dbfs(heard),
                                                               aa.peak_dbfs(changed)))
    if recorder is not None and os.path.exists(air):
        in_air, r3 = aa.read_wav(air)
        loud = max((aa.dbfs(in_air[i:i + 8000]) for i in range(0, max(1, len(in_air) - 8000), 4000)),
                   default=-120)
        checks.check(label + " heard in the air", loud > -45, "loudest 0.5 s at %.1f dBFS" % loud)
        ratio_air, _, f_air = aa.pitch_ratio(heard, in_air, r1)
        checks.check(label + " pitch in the air", ratio_air > 0 and abs(ratio_air / want - 1) <= 0.12,
                     "x%.2f (want x%.2f): %.0f Hz" % (ratio_air, want, f_air))
        if out:
            os.replace(air, os.path.join(out, label.replace("/", "_") + "_air.wav"))
    if out:
        base = os.path.join(out, label.replace("/", "_"))
        aa.write_wav(base + "_heard.wav", heard, r1)
        aa.write_wav(base + "_said.wav", changed, r2)


def run_click(board, checks, tmp):
    """A 60 ms beep must not count as a phrase."""
    path = os.path.join(tmp, "click.wav")
    beep = [int(12000 * math.sin(2 * math.pi * 900 * i / 16000)) for i in range(960)]
    aa.write_wav(path, [0] * 1600 + beep + [0] * 1600, 16000)
    ensure_listening(board)
    start = time.time()
    subprocess.run(["afplay", path])
    board.pump(2.0)
    started = [l for t, l in board.events if t >= start and l.startswith("EV heard start")]
    ended = [l for t, l in board.events if t >= start and l.startswith("EV heard end")]
    discarded = [l for t, l in board.events if t >= start and l.startswith("EV heard discard")]
    checks.check("click is not a phrase", not ended,
                 "start %d, discard %d, end %d" % (len(started), len(discarded), len(ended)))


def run_silence(board, checks, seconds):
    ensure_listening(board)
    start = time.time()
    board.pump(seconds)
    caught = [l for t, l in board.events if t >= start and l.startswith("EV heard start")]
    checks.check("silence stays silent", not caught, "%d false starts in %d s" % (len(caught), seconds))


def run_frame_rate(board, checks):
    board.send("perf")
    start = time.time()
    board.pump(3.2)
    board.send("perf")
    board.pump(0.2)
    lines = [l for t, l in board.events if t >= start and l.startswith("perf ")]
    if not lines:
        checks.check("frame rate", False, "no perf lines")
        return
    fps = [int(l.split()[6]) for l in lines]
    paint = [int(l.split()[2]) for l in lines]
    push = [int(l.split()[4]) for l in lines]
    checks.check("frame rate", min(fps) >= 25,
                 "fps %s, paint %d us, push %d us" % (fps, max(paint), max(push)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chars", default="0,1,2", help="characters to test, e.g. 0,2")
    ap.add_argument("--quick", action="store_true", help="two phrases per character")
    ap.add_argument("--out", help="folder for WAVs and screenshots")
    ap.add_argument("--quiet", action="store_true", help="do not print the board's events")
    ap.add_argument("--mic", action="store_true", help="record the board's speaker with the Mac's mic")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="talk_test_")
    if args.out:
        os.makedirs(args.out, exist_ok=True)
    board = Board(verbose=not args.quiet)
    checks = Checks()
    board.pump(0.5)
    phrases = [p for p in PHRASES if not args.quick or p[0] in QUICK]
    mic = find_mac_mic() if args.mic else None

    print("== silence and clicks")
    run_silence(board, checks, 6 if args.quick else 10)
    run_click(board, checks, tmp)
    run_frame_rate(board, checks)
    for char in [int(c) for c in args.chars.split(",")]:
        print("== %s" % NAMES[char])
        if not select(board, char):
            checks.check(NAMES[char] + " selected", False, "the board did not switch")
            continue
        for case in phrases:
            run_phrase(board, checks, char, tmp, args.out, case, mic)
    select(board, 0)

    print("\n%d checks, %d failed" % (len(checks.rows), checks.failed))
    sys.exit(1 if checks.failed else 0)


if __name__ == "__main__":
    main()
