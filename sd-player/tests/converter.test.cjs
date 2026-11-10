const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../audio-converter.html'), 'utf8');
const context = vm.createContext({ document: { getElementById() { return {}; } } });
vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);

test('converter writes standard PCM16 stereo 44100 Hz RIFF WAV', () => {
  const channels = [new Float32Array([0, -1, 1]), new Float32Array([1, 0, -1])];
  const data = context.encodeWav({ length: 3, getChannelData: i => channels[i] });
  const v = new DataView(data);
  assert.equal(Buffer.from(data, 0, 4).toString(), 'RIFF');
  assert.equal(Buffer.from(data, 8, 4).toString(), 'WAVE');
  assert.equal(Buffer.from(data, 12, 4).toString(), 'fmt ');
  assert.equal(Buffer.from(data, 36, 4).toString(), 'data');
  assert.equal(v.getUint32(4, true), data.byteLength - 8);
  assert.equal(v.getUint16(20, true), 1);
  assert.equal(v.getUint16(22, true), 2);
  assert.equal(v.getUint32(24, true), 44100);
  assert.equal(v.getUint32(28, true), 176400);
  assert.equal(v.getUint16(32, true), 4);
  assert.equal(v.getUint16(34, true), 16);
  assert.equal(v.getUint32(40, true), 12);
  assert.deepEqual(Array.from({ length: 6 }, (_, i) => v.getInt16(44 + i * 2, true)),
    [0, 32767, -32768, 0, 32767, -32768]);
});
test('converter clips out-of-range samples and silences non-finite values', () => {
  const samples = new Float32Array([2, -2, NaN, Infinity]);
  const data = context.encodeWav({ length: 4, getChannelData: () => samples });
  const v = new DataView(data);
  assert.deepEqual(Array.from({ length: 4 }, (_, i) => v.getInt16(44 + i * 4, true)),
    [32767, -32768, 0, 0]);
});
