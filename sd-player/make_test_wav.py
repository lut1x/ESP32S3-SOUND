"""Generate a short L/R test: left 440 Hz, silence, right 660 Hz."""
import math
from pathlib import Path
import struct
import wave

target = Path(__file__).with_name("test-lr.wav")
rate = 44100
with wave.open(str(target), "wb") as wav:
    wav.setnchannels(2)
    wav.setsampwidth(2)
    wav.setframerate(rate)
    frames = bytearray()
    for i in range(rate * 3):
        t = i / rate
        left = right = 0
        if t < 1 or 2 <= t < 3:
            local = t if t < 1 else t - 2
            envelope = min(1.0, local / 0.02, (1 - local) / 0.02)
            sample = int(8000 * envelope * math.sin(2 * math.pi * (440 if t < 1 else 660) * local))
            if t < 1:
                left = sample
            else:
                right = sample
        frames.extend(struct.pack("<hh", left, right))
    wav.writeframes(frames)
print(target)
