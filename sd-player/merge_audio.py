"""Put two recordings in separate stereo channels for the ESP32 SD player.

First input -> mono -> LEFT. Second input -> mono -> RIGHT.
Output: standard RIFF WAV, signed PCM16, stereo, 44100 Hz.
PCM WAV inputs work with Python alone; other formats require FFmpeg.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 44100
BLOCK = 4096
MAX_FILE_BYTES = 2147483647  # Upload/player limit in this project.


class MonoSource:
    """Read PCM WAV in bounded chunks and average its channels without gain."""

    def __init__(self, path: Path):
        self.wav = wave.open(str(path), "rb")
        self.channels = self.wav.getnchannels()
        self.width = self.wav.getsampwidth()
        self.rate = self.wav.getframerate()
        self.frames = self.wav.getnframes()
        if (self.wav.getcomptype() != "NONE" or self.width not in (1, 2, 3, 4)
                or not 1 <= self.channels <= 32 or self.rate <= 0 or not self.frames):
            self.close()
            raise ValueError(f"Неподдерживаемый или пустой PCM WAV: {path.name}")
        self.output_frames = (self.frames * RATE + self.rate - 1) // self.rate
        self.start = -1
        self.samples: list[float] = []

    def close(self):
        self.wav.close()

    def sample(self, index: int) -> float:
        index = min(index, self.frames - 1)
        if not self.start <= index < self.start + len(self.samples):
            self.start = index // BLOCK * BLOCK
            self.wav.setpos(self.start)
            expected = min(BLOCK, self.frames - self.start)
            data = self.wav.readframes(expected)
            stride = self.width * self.channels
            if len(data) != expected * stride:
                raise ValueError("Исходный WAV обрезан: аудиоданных меньше, чем в заголовке.")
            self.samples = []
            scale = 2 ** (16 - self.width * 8)
            for offset in range(0, len(data), stride):
                total = 0
                for channel in range(self.channels):
                    begin = offset + channel * self.width
                    if self.width == 1:
                        total += data[begin] - 128  # 8-bit WAV is unsigned.
                    else:
                        total += int.from_bytes(data[begin:begin + self.width], "little", signed=True)
                self.samples.append(total * scale / self.channels)
        return self.samples[index - self.start]

    def output_sample(self, index: int, loop: bool) -> int:
        if index >= self.output_frames:
            if not loop:
                return 0
            index %= self.output_frames
        frame, fraction = divmod(index * self.rate, RATE)
        first = self.sample(frame)
        value = first
        if fraction:
            value += (self.sample(frame + 1) - first) * fraction / RATE
        return max(-32768, min(32767, round(value)))


def find_ffmpeg(explicit: str | None) -> str | None:
    if explicit:
        candidate = Path(explicit).expanduser()
        found = str(candidate.resolve()) if candidate.is_file() else shutil.which(explicit)
        if not found:
            raise ValueError(f"FFmpeg не найден: {explicit}")
        return found
    for name in ("ffmpeg.exe", "ffmpeg"):
        candidate = Path(__file__).with_name(name)
        if candidate.is_file():
            return str(candidate)
    return shutil.which("ffmpeg")


def prepare_source(path: Path, temporary: Path, ffmpeg: str | None) -> MonoSource:
    if not path.is_file():
        raise ValueError(f"Файл не найден: {path}")
    # FFmpeg supplies high quality resampling and decoders when installed.
    if ffmpeg:
        result = subprocess.run([
            ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
            "-i", str(path), "-map", "0:a:0", "-vn", "-ac", "1", "-ar", str(RATE),
            "-c:a", "pcm_s16le", str(temporary),
        ], capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
        if result.returncode:
            raise ValueError(f"Не удалось декодировать {path.name}:\n{result.stderr.strip()}")
        return MonoSource(temporary)
    try:
        return MonoSource(path)
    except (wave.Error, EOFError) as error:
        raise ValueError(
            f"Не удалось открыть {path.name} как PCM WAV. Для MP3/FLAC/float WAV и других "
            "форматов нужен FFmpeg в PATH, рядом со скриптом или через --ffmpeg. "
            "Либо сначала преобразуйте исходники через audio-converter.html."
        ) from error


def merge_audio(left: Path, right: Path, output: Path, *, loop_shorter: bool = False,
                ffmpeg: str | None = None) -> tuple[int, int]:
    left, right, output = (Path(p).expanduser().resolve() for p in (left, right, output))
    if output in (left, right):
        raise ValueError("Выходной файл не должен совпадать с исходниками.")
    if output.suffix.lower() != ".wav":
        raise ValueError("Укажите расширение .wav для выходного файла.")
    if output.exists():
        raise ValueError(f"Файл уже существует: {output}. Укажите новое имя — перезапись отключена.")
    created = False
    try:
        with tempfile.TemporaryDirectory(prefix="soundboard-merge-") as folder, ExitStack() as stack:
            directory = Path(folder)
            sources = []
            for index, path in enumerate((left, right)):
                source = prepare_source(path, directory / f"input-{index}.wav", ffmpeg)
                stack.callback(source.close)
                sources.append(source)
            frames = max(source.output_frames for source in sources)
            if frames * 4 + 44 > MAX_FILE_BYTES:
                raise ValueError("Результат больше 2 ГБ. Сократите записи для загрузки в SD-плеер.")
            # Exclusive create protects existing files, including a creation race.
            with output.open("xb") as stream:
                created = True
                with wave.open(stream, "wb") as writer:
                    writer.setnchannels(2)
                    writer.setsampwidth(2)
                    writer.setframerate(RATE)
                    for first in range(0, frames, BLOCK):
                        count = min(BLOCK, frames - first)
                        data = bytearray(count * 4)
                        for i in range(count):
                            position = first + i
                            struct.pack_into("<hh", data, i * 4,
                                             sources[0].output_sample(position, loop_shorter),
                                             sources[1].output_sample(position, loop_shorter))
                        writer.writeframesraw(data)
        with wave.open(str(output), "rb") as check:
            if (check.getnchannels(), check.getsampwidth(), check.getframerate(), check.getnframes()) != (2, 2, RATE, frames):
                raise ValueError("Проверка результата WAV не прошла.")
        return frames, output.stat().st_size
    except BaseException:
        if created:
            output.unlink(missing_ok=True)
        raise


def main() -> int:
    # Windows consoles can use CP1251: names outside that codepage must not crash
    # processing or the final success message (filesystem paths stay Unicode).
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(
        description="Две записи в отдельных L/R каналах WAV PCM16 stereo 44100 Гц.",
        epilog="Пример: python merge_audio.py left.wav right.wav combined.wav",
    )
    parser.add_argument("left", type=Path, help="первая запись — в ЛЕВЫЙ канал")
    parser.add_argument("right", type=Path, help="вторая запись — в ПРАВЫЙ канал")
    parser.add_argument("output", type=Path, help="итоговый WAV, новое имя файла")
    parser.add_argument("--loop-shorter", action="store_true",
                        help="повторять короткую запись до длины длинной вместо добавления тишины")
    parser.add_argument("--ffmpeg", help="путь к ffmpeg.exe, если его нет в PATH")
    args = parser.parse_args()
    try:
        ffmpeg = find_ffmpeg(args.ffmpeg)
        print(f"Левый канал: {args.left}\nПравый канал: {args.right}")
        print("Обработка FFmpeg…" if ffmpeg else "Обработка PCM WAV средствами Python…")
        frames, size = merge_audio(args.left, args.right, args.output,
                                   loop_shorter=args.loop_shorter, ffmpeg=ffmpeg)
    except (OSError, ValueError, wave.Error) as error:
        print(f"Ошибка: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Операция отменена.", file=sys.stderr)
        return 130
    print(f"Готово: {args.output.resolve()}\nWAV PCM16 · stereo · 44100 Гц · {frames / RATE:.2f} с · {size / 1048576:.2f} МБ")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
