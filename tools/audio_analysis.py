"""Small audio helpers for the self-tests: WAV I/O, levels, pitch.

Plain Python, so it runs under PlatformIO's own interpreter (which has
pyserial but no numpy); numpy is used when it happens to be there.
"""

import math
import struct
import wave

try:
    import numpy as np
except ImportError:  # PlatformIO's Python
    np = None


def read_wav(path):
    """Returns (samples as a list of ints, sample rate); mono 16-bit only."""
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise ValueError(f"{path}: need 16-bit mono")
        rate = w.getframerate()
        data = w.readframes(w.getnframes())
    return list(struct.unpack(f"<{len(data) // 2}h", data)), rate


def write_wav(path, samples, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def dbfs(samples):
    """RMS level in dBFS; -120 for silence."""
    if not samples:
        return -120.0
    acc = sum(s * s for s in samples)
    if acc == 0:
        return -120.0
    return 20 * math.log10(math.sqrt(acc / len(samples)) / 32768.0)


def peak_dbfs(samples):
    p = max((abs(s) for s in samples), default=0)
    return 20 * math.log10(p / 32768.0) if p else -120.0


def voiced_bounds(samples, rate, frame_ms=20, above_db=-35.0):
    """(start, end) in seconds of the part louder than above_db relative
    to the loudest frame; (0, 0) for silence."""
    n = rate * frame_ms // 1000
    levels = [dbfs(samples[i:i + n]) for i in range(0, len(samples) - n + 1, n)]
    if not levels:
        return 0.0, 0.0
    top = max(levels)
    loud = [i for i, l in enumerate(levels) if l >= top + above_db]
    if not loud:
        return 0.0, 0.0
    return loud[0] * frame_ms / 1000.0, (loud[-1] + 1) * frame_ms / 1000.0


def voiced_span(samples, rate, frame_ms=20, above_db=-35.0):
    """Seconds from the first to the last loud frame."""
    start, end = voiced_bounds(samples, rate, frame_ms, above_db)
    return end - start


def _decimate2(samples):
    # A 2-tap average is enough of a low-pass for pitch tracking.
    return [(samples[i] + samples[i + 1]) / 2 for i in range(0, len(samples) - 1, 2)]


def pitch_track(samples, rate, fmin=60.0, fmax=900.0, frame_ms=40, hop_ms=20):
    """Fundamental frequency of each voiced frame (normalised
    autocorrelation), in Hz. Works at rate / 2 for speed."""
    x = _decimate2(samples)
    r = rate / 2
    n = int(r * frame_ms / 1000)
    hop = int(r * hop_ms / 1000)
    lag_min = max(2, int(r / fmax))
    lag_max = int(r / fmin)
    loud_floor = max(dbfs(samples) - 12, -50)
    out = []
    for start in range(0, len(x) - n - lag_max, hop):
        frame = x[start:start + n + lag_max]
        if dbfs([int(v) for v in frame[:n]]) < loud_floor:
            continue
        if np is not None:
            f = np.asarray(frame, dtype=np.float64)
            a = f[:n]
            e0 = float(np.dot(a, a))
            corr = [float(np.dot(a, f[lag:lag + n])) / math.sqrt(e0 * float(np.dot(f[lag:lag + n], f[lag:lag + n])) + 1e-9)
                    for lag in range(lag_min, lag_max + 1)]
        else:
            a = frame[:n]
            e0 = sum(v * v for v in a)
            corr = []
            for lag in range(lag_min, lag_max + 1):
                b = frame[lag:lag + n]
                corr.append(sum(p * q for p, q in zip(a, b)) / math.sqrt(e0 * sum(v * v for v in b) + 1e-9))
        best = max(corr)
        # The first peak nearly as high as the best one: the best is often
        # at twice the period.
        best_lag = 0
        for i in range(1, len(corr) - 1):
            if corr[i] >= 0.9 * best and corr[i] >= corr[i - 1] and corr[i] >= corr[i + 1]:
                best_lag = lag_min + i
                best = corr[i]
                break
        if best > 0.6 and best_lag > lag_min:
            out.append(r / best_lag)
    return out


def median(values):
    v = sorted(values)
    if not v:
        return 0.0
    m = len(v) // 2
    return v[m] if len(v) % 2 else (v[m - 1] + v[m]) / 2


def pitch_ratio(original, changed, rate):
    """Median pitch of `changed` over median pitch of `original`, with both
    medians; (0, a, b) when either has no voiced frames."""
    a = median(pitch_track(original, rate))
    b = median(pitch_track(changed, rate))
    if a <= 0 or b <= 0:
        return 0.0, a, b
    return b / a, a, b
