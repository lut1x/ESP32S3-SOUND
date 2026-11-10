import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import wave

spec = importlib.util.spec_from_file_location("merge_audio", Path(__file__).parents[1] / "merge_audio.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class MergeAudioTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name)

    def source(self, name, frames, channels=1, width=2, rate=44100):
        path = self.folder / name
        data = bytearray()
        for frame in frames:
            values = (frame,) if channels == 1 else frame
            for value in values:
                data.extend(value.to_bytes(width, "little", signed=width != 1))
        with wave.open(str(path), "wb") as writer:
            writer.setnchannels(channels)
            writer.setsampwidth(width)
            writer.setframerate(rate)
            writer.writeframes(data)
        return path

    def result(self, left, right, **kwargs):
        output = self.folder / "result.wav"
        count, size = module.merge_audio(left, right, output, **kwargs)
        with wave.open(str(output), "rb") as reader:
            self.assertEqual((reader.getnchannels(), reader.getsampwidth(), reader.getframerate()), (2, 2, 44100))
            self.assertEqual(reader.getnframes(), count)
            self.assertEqual(size, count * 4 + 44)
            return list(struct.iter_unpack("<hh", reader.readframes(count)))

    def test_stereo_sources_are_averaged_then_routed_separately(self):
        left = self.source("left.wav", [(1000, 3000), (-1000, -3000)], channels=2)
        right = self.source("right.wav", [-4000, 6000])
        self.assertEqual(self.result(left, right), [(2000, -4000), (-2000, 6000)])

    def test_short_source_is_padded_with_silence(self):
        left = self.source("left.wav", [123])
        right = self.source("right.wav", [10, 20, 30])
        self.assertEqual(self.result(left, right), [(123, 10), (0, 20), (0, 30)])

    def test_short_source_can_repeat_instead(self):
        left = self.source("left.wav", [123, -123])
        right = self.source("right.wav", [10, 20, 30, 40, 50])
        self.assertEqual(self.result(left, right, loop_shorter=True),
                         [(123, 10), (-123, 20), (123, 30), (-123, 40), (123, 50)])

    def test_different_rates_are_converted_to_44100(self):
        left = self.source("left.wav", [0, 1000], rate=22050)
        right = self.source("right.wav", [0, 0, 0, 0])
        self.assertEqual(self.result(left, right), [(0, 0), (500, 0), (1000, 0), (1000, 0)])

    def test_24bit_and_unsigned_8bit_inputs(self):
        left = self.source("left.wav", [256000, -256000, 0x7fffff], width=3)
        right = self.source("right.wav", [128, 0, 255], width=1)
        self.assertEqual(self.result(left, right), [(1000, 0), (-1000, -32768), (32767, 32512)])

    def test_existing_output_and_inputs_are_preserved(self):
        left = self.source("left.wav", [100])
        right = self.source("right.wav", [200])
        original = left.read_bytes()
        with self.assertRaises(ValueError):
            module.merge_audio(left, right, left)
        self.assertEqual(left.read_bytes(), original)
        output = self.folder / "existing.wav"
        output.write_bytes(b"keep me")
        with self.assertRaises(ValueError):
            module.merge_audio(left, right, output)
        self.assertEqual(output.read_bytes(), b"keep me")

    def test_truncated_source_removes_incomplete_output(self):
        left = self.source("left.wav", [100, 200])
        left.write_bytes(left.read_bytes()[:-2])
        right = self.source("right.wav", [300, 400])
        output = self.folder / "result.wav"
        with self.assertRaises(ValueError):
            module.merge_audio(left, right, output)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
